import qbs

Application {
	Depends { name: "cpp" }

	property string s_arch: "armv7l-unknown-linux-gnueabihf"

	cpp.cFlags: ["-pthread", "-target", s_arch]
	cpp.cxxFlags: ["-pthread", "-target", s_arch, "-std=c++14"]
	cpp.linkerFlags: ["-pthread", "-fuse-ld=gold", "-target", s_arch]

	cpp.includePaths: [
		cpp.sysroot + "/usr/include",
		cpp.sysroot + "/opt/vc/include",
		cpp.sysroot + "/opt/vc/include/interface/vcos/pthreads",
		cpp.sysroot + "/opt/vc/include/interface/vmcs_host/linux",
	]

	cpp.libraryPaths: [cpp.sysroot + "/opt/vc/lib"]

	cpp.staticLibraries: ["GLESv2", "EGL", "openmaxil", "bcm_host", "vcos", "vchiq_arm", "mmal_core", "mmal_vc_client", "mmal_util", "mmal_components", "mmal"]

	files: [
        "main.cpp",
	]

	Group {
		fileTagsFilter: product.type
		qbs.install: true
		qbs.installDir: "usr/local/bin"
	}
}
