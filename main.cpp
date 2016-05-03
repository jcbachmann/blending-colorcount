#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_util.h"


//#define HISTOGRAM

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

uint8_t* picture;
std::mutex pictureMutexes[4];
std::condition_variable newPicture;
std::mutex newPictureMutex;
std::vector<std::thread> procthreads;
uint32_t results[4][4];
int64_t prevtimestamp;

typedef struct {
	int video_width;
	int video_height;
	int preview_width;
	int preview_height;
	float video_fps;
	MMAL_POOL_T *camera_video_port_pool;
	VCOS_SEMAPHORE_T complete_semaphore;
} PORT_USERDATA;

const unsigned width = 2592, height = 1944;

static void video_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	static int frame_count = 0;
	static int frame_post_count = 0;
	MMAL_BUFFER_HEADER_T *new_buffer;
	PORT_USERDATA * userdata = (PORT_USERDATA *) port->userdata;
	MMAL_POOL_T *pool = userdata->camera_video_port_pool;

	mmal_buffer_header_mem_lock(buffer);
	for (std::mutex& pictureMutex : pictureMutexes)
		pictureMutex.lock();
	memcpy(picture, buffer->data, width*height);
	memcpy(picture + width*height/4*4, buffer->data + 6718464, width*height/4);
	memcpy(picture + width*height/4*5, buffer->data + 8398080, width*height/4);
	unsigned sumresults[4] = {0};
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			sumresults[i] += results[j][i];
		}
	}
	for (std::mutex& pictureMutex : pictureMutexes)
		pictureMutex.unlock();
	newPicture.notify_all();
	mmal_buffer_header_mem_unlock(buffer);

	//fprintf(stderr, "img = %d w=%d, h=%d\n", img, img->width, img->height);

	frame_count++;
	if (vcos_semaphore_trywait(&(userdata->complete_semaphore)) != VCOS_SUCCESS) {
		vcos_semaphore_post(&(userdata->complete_semaphore));
		frame_post_count++;
	}

#ifndef HISTOGRAM
	std::cout << std::setw(5) << frame_count << ' ' << std::setw(5) << std::setw(10) << prevtimestamp << ' ';
	for (unsigned r : sumresults) {
		std::cout << std::setw(8) << r/float(width*height) << ' ';
	}
	std::cout << std::setw(8) << 1.f - (sumresults[0] + sumresults[1] + sumresults[2] + sumresults[3])/float(width*height) << ' ';
	std::cout << std::endl;
#endif

	prevtimestamp = buffer->pts;

	mmal_buffer_header_release(buffer);
	// and send one back to the port (if still open)
	if (port->is_enabled) {
		MMAL_STATUS_T status;

		new_buffer = mmal_queue_get(pool->queue);

		if (new_buffer)
			status = mmal_port_send_buffer(port, new_buffer);

		if (!new_buffer || status != MMAL_SUCCESS)
			fprintf(stderr, "Unable to return a buffer to the video port\n");
	}
}

volatile bool running = true;

void stop(int)
{
	running = false;
}

enum Results
{
	gray,
	red,
	blue,
	yellow,
};

void procthread(int c)
{
	int startLine = height/4 * c;
	int endLine = height/4 * (c+1);
	uint8_t* daty = picture + (width*height/4 * 0);
	uint8_t* datu = picture + (width*height/4 * 4);
	uint8_t* datv = picture + (width*height/4 * 5);
	while (running) {
		std::unique_lock<std::mutex> l1(newPictureMutex);
		newPicture.wait(l1);
		std::lock_guard<std::mutex> l2(pictureMutexes[c]);
		results[c][gray] = 0;
		results[c][red] = 0;
		results[c][blue] = 0;
		results[c][yellow] = 0;
		for (int y = startLine; y != endLine; y+=2) {
			for (int x = 0; x < width; x+=2) {
				uint8_t yval1 = daty[y*(width)+x];
				uint8_t yval2 = daty[y*(width)+x+1];
				uint8_t yval3 = daty[(y+1)*(width)+x];
				uint8_t yval4 = daty[(y+1)*(width)+x+1];
				uint8_t uval = datu[(y/2)*(width/2)+(x/2)];
				uint8_t vval = datv[(y/2)*(width/2)+(x/2)];
				if (uval >= 120 && uval <= 146 && vval >= 100 && vval <= 160) {
					results[c][gray]+=4;
				} else if (uval < 128 && vval >= 152) {
					results[c][red]+=4;
				} else if (uval > 146 && vval < 128) {
					results[c][blue]+=4;
				} else if (uval < 120 && vval > 104 && vval <= 136) {
					results[c][yellow]+=4;
				} else if (uval >= 112 && uval <= 120 && vval >= 128 && vval < 152) {
					results[c][gray]+=4;
				}
			}
		}
		if (c == 0) {
			/*for (int i = 1; i < width*height*2; i++) {
				if (picture[i] == 0 && picture[i-1] != 0) {
					std::cout << i << std::endl;
				}
				if (picture[i] != 0 && picture[i-1] == 0) {
					std::cout << i << std::endl;
				}
			}*/
#ifdef HISTOGRAM
			unsigned a[64][64] = {{0}};
			for (int y = 0; y < height; y+=2) {
				for (int x = 0; x < width; x+=2) {
					if (daty[y*(width)+x] > 30 && daty[y*(width)+x] < 220) {
						uint8_t uval = datu[(y/2)*(width/2)+(x/2)];
						uint8_t vval = datv[(y/2)*(width/2)+(x/2)];
						if (!(uval >= 120 && uval <= 146 && vval >= 100 && vval <= 160 || uval < 128 && vval > 160 || uval > 146 && vval < 128 || uval < 120 && vval > 104 && vval <= 136))
						{
							a[uval>>3][vval>>3]++;
						}

					//uint8_t uval = datu[(y/2)*(width/2)+(x/2)];
					//uint8_t vval = datv[(y/2)*(width/2)+(x/2)];
					//a[y>>7][x>>8]+=daty[y*(width)+x];
					//a[y>>7][x>>8]+=vval;
					}
				}
			}
			std::cout << std::setw(4) << 0;
			for (int j = 0; j < 16; j++) {
				std::cout << std::setw(8) << ((j+10)<<3);
			}
			std::cout << '\n';
			for (int i = 0; i < 16; i++) {
				std::cout << std::setw(4) << ((i+8)<<3);
				for (int j = 0; j < 16; j++) {
					std::cout << std::setw(8) << a[i+8][j+10];
				}
				std::cout << '\n';
			}
			std::cout << std::endl;
#endif
		}
	}
}

int main(int argc, char** argv) {
	MMAL_COMPONENT_T *camera = 0;
	MMAL_COMPONENT_T *preview = 0;
	MMAL_ES_FORMAT_T *format;
	MMAL_STATUS_T status;
	MMAL_PORT_T *camera_preview_port = NULL, *camera_video_port = NULL, *camera_still_port = NULL;
	MMAL_PORT_T *preview_input_port = NULL;
	MMAL_POOL_T *camera_video_port_pool;
	MMAL_CONNECTION_T *camera_preview_connection = 0;
	PORT_USERDATA userdata;

	std::cout << std::fixed;

	signal(SIGTERM, stop);

	bcm_host_init();

	userdata.preview_width = 2592/2;
	userdata.preview_height = 1944/2;
	userdata.video_width = 2592;
	userdata.video_height = 1944;

	picture = new uint8_t[userdata.video_width * userdata.video_height / 4 * 6];
	for (int i = 0; i < 4; i++) {
		procthreads.emplace_back(procthread, procthreads.size());
	}

	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: create camera %x\n", status);
		return -1;
	}

	camera_preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
	camera_video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
	//camera_video_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

	{
		MMAL_PARAMETER_CAMERA_CONFIG_T cam_config;
		cam_config.hdr.id = MMAL_PARAMETER_CAMERA_CONFIG;
		cam_config.hdr.size = sizeof(cam_config);
		cam_config.max_stills_w = 2592;
		cam_config.max_stills_h = 1944;
		cam_config.stills_yuv422 = 0;
		cam_config.one_shot_stills = 0;
		cam_config.max_preview_video_w = 2592;
		cam_config.max_preview_video_h = 1944;
		cam_config.num_preview_video_frames = 3;
		cam_config.stills_capture_circular_buffer_height = 0;
		cam_config.fast_preview_resume = 0;
		cam_config.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC;

		status = mmal_port_parameter_set(camera->control, &cam_config.hdr);

		if (status != MMAL_SUCCESS) {
			fprintf(stderr, "Error: unable to set camera config (%u)\n", status);
			return -1;
		}
	}

	{
		MMAL_PARAMETER_CAMERA_CONFIG_T cam_config;
		cam_config.hdr.id = MMAL_PARAMETER_CAMERA_CONFIG;
		cam_config.hdr.size = sizeof(cam_config);
		cam_config.max_stills_w = 2592;
		cam_config.max_stills_h = 1944;
		cam_config.stills_yuv422 = 0;
		cam_config.one_shot_stills = 0;
		cam_config.max_preview_video_w = 2592;
		cam_config.max_preview_video_h = 1944;
		cam_config.num_preview_video_frames = 3;
		cam_config.stills_capture_circular_buffer_height = 0;
		cam_config.fast_preview_resume = 0;
		cam_config.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC;

		status = mmal_port_parameter_set(camera->control, &cam_config.hdr);

		if (status != MMAL_SUCCESS) {
			fprintf(stderr, "Error: unable to set camera config (%u)\n", status);
			return -1;
		}

		status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_AWB_MODE, MMAL_PARAM_AWBMODE_OFF);

		if (status != MMAL_SUCCESS) {
			fprintf(stderr, "Error: unable to set camera awbmode (%u)\n", status);
			return -1;
		}

		MMAL_PARAMETER_AWB_GAINS_T awbgains;
		awbgains.hdr.id = MMAL_PARAMETER_CUSTOM_AWB_GAINS;
		awbgains.hdr.size = sizeof(awbgains);
		awbgains.r_gain.num = 7;
		awbgains.r_gain.den = 4;
		awbgains.b_gain.num = 7;
		awbgains.b_gain.den = 4;

		status = mmal_port_parameter_set(camera->control, &awbgains.hdr);

		if (status != MMAL_SUCCESS) {
			fprintf(stderr, "Error: unable to set camera settings (%u)\n", status);
			return -1;
		}
	}

	format = camera_video_port->format;

	format->encoding = MMAL_ENCODING_I420;
	format->encoding_variant = MMAL_ENCODING_I420;

	format->es->video.width = userdata.video_width;
	format->es->video.height = userdata.video_width;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = userdata.video_width;
	format->es->video.crop.height = userdata.video_height;
	format->es->video.frame_rate.num = 0;
	format->es->video.frame_rate.den = 1;

	camera_video_port->buffer_size = userdata.preview_width * userdata.preview_height * 12 / 8;
	camera_video_port->buffer_num = 1;

	status = mmal_port_format_commit(camera_video_port);

	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: unable to commit camera video port format (%u)\n", status);
		return -1;
	}

	format = camera_preview_port->format;

	format->encoding = MMAL_ENCODING_OPAQUE;
	format->encoding_variant = MMAL_ENCODING_I420;

	format->es->video.width = userdata.preview_width;
	format->es->video.height = userdata.preview_height;
	format->es->video.crop.x = 0;
	format->es->video.crop.y = 0;
	format->es->video.crop.width = userdata.preview_width;
	format->es->video.crop.height = userdata.preview_height;

	status = mmal_port_parameter_set_boolean(camera_preview_port, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);

	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: MMAL_PARAMETER_ZERO_COPY (%u)\n", status);
		return -1;
	}

	status = mmal_port_parameter_set_boolean(camera_preview_port, MMAL_PARAMETER_CAMERA_BURST_CAPTURE, 1);

	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: MMAL_PARAMETER_ZERO_COPY (%u)\n", status);
		return -1;
	}

	status = mmal_port_format_commit(camera_preview_port);

	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: camera preview format couldn't be set\n");
		return -1;
	}

	// crate pool form camera video port
	camera_video_port_pool = (MMAL_POOL_T *) mmal_port_pool_create(camera_video_port, camera_video_port->buffer_num, camera_video_port->buffer_size);
	userdata.camera_video_port_pool = camera_video_port_pool;
	camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *) &userdata;

	status = mmal_port_enable(camera_video_port, video_buffer_callback);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: unable to enable camera video port (%u)\n", status);
		return -1;
	}



	status = mmal_component_enable(camera);

	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &preview);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: unable to create preview (%u)\n", status);
		return -1;
	}
	preview_input_port = preview->input[0];

	{
		MMAL_DISPLAYREGION_T param;
		param.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
		param.hdr.size = sizeof (MMAL_DISPLAYREGION_T);
		param.set = MMAL_DISPLAY_SET_LAYER;
		param.layer = 0;
		param.set |= MMAL_DISPLAY_SET_FULLSCREEN;
		param.fullscreen = 1;
		status = mmal_port_parameter_set(preview_input_port, &param.hdr);
		if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
			fprintf(stderr, "Error: unable to set preview port parameters (%u)\n", status);
			return -1;
		}
	}

	status = mmal_connection_create(&camera_preview_connection, camera_preview_port, preview_input_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: unable to create connection (%u)\n", status);
		return -1;
	}

	status = mmal_connection_enable(camera_preview_connection);
	if (status != MMAL_SUCCESS) {
		fprintf(stderr, "Error: unable to enable connection (%u)\n", status);
		return -1;
	}

	if (1) {
		// Send all the buffers to the camera video port
		int num = mmal_queue_length(camera_video_port_pool->queue);
		int q;

		for (q = 0; q < num; q++) {
			MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(camera_video_port_pool->queue);

			if (!buffer) {
				fprintf(stderr, "Unable to get a required buffer %d from pool queue\n", q);
			}

			if (mmal_port_send_buffer(camera_video_port, buffer) != MMAL_SUCCESS) {
				fprintf(stderr, "Unable to send a buffer to encoder output port (%d)\n", q);
			}
		}

	}

	if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
		fprintf(stderr, "%s: Failed to start capture\n", __func__);
	}

	vcos_semaphore_create(&userdata.complete_semaphore, "mmal_opencv_demo-sem", 0);

	while (running) {
		if (vcos_semaphore_wait(&(userdata.complete_semaphore)) == VCOS_SUCCESS) {
		}
	}

	for (std::thread& t : procthreads) {
		t.join();
	}

	return 0;
}
