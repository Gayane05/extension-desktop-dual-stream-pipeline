# desktop/recipes/sherpa-onnx/conanfile.py
#
# In-repo Conan 2 recipe for sherpa-onnx (https://github.com/k2-fsa/sherpa-onnx).
# There is no sherpa-onnx recipe on ConanCenter, so we vendor one here.
#
# Build it into the local cache with (run from this directory):
#
#   conan create . --version 1.13.5 \
#       --profile:all=../../conan_profiles/default --build=missing -s build_type=Release
#
# Use the project profile so the package is built for the same
# compiler/arch/runtime as the app. Its compiler.cppstd=20 is deliberately
# dropped again in configure() below -- see the comment there.
import os

from conan import ConanFile
from conan.errors import ConanException, ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rm


class SherpaOnnxConan(ConanFile):
    name = "sherpa-onnx"
    # version is passed on the command line: conan create . --version <V>
    package_type = "library"
    license = "Apache-2.0"
    homepage = "https://github.com/k2-fsa/sherpa-onnx"
    url = "https://github.com/k2-fsa/sherpa-onnx"
    description = "Speech-to-text/ASR runtime built on onnxruntime (C API used by this app)"
    topics = ("asr", "speech-to-text", "onnxruntime", "sherpa")

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "cuda": [True, False],
        "tts": [True, False],
    }
    default_options = {
        "shared": True,
        # CPU only. `cuda` is plumbed to SHERPA_ONNX_ENABLE_GPU but the CUDA
        # build is not exercised by this project.
        "cuda": False,
        # We only need ASR. Turning TTS off skips the espeak-ng /
        # piper-phonemize subtrees, which is a large chunk of the build.
        # Upstream ships prebuilt "no-tts" archives, so this is a supported
        # configuration.
        "tts": False,
    }

    # sherpa-onnx pulls ~8 third-party archives from GitHub at *configure* time
    # via FetchContent. CMake's bundled curl intermittently dies on those with
    # "HTTP/2 REFUSED_STREAM ... tried 5 times before giving up", which aborts
    # the whole configure. FetchContent keeps its ExternalProject stamps in the
    # build directory, so a re-run skips everything already downloaded and only
    # retries what is missing: retrying configure converges quickly.
    _configure_attempts = 5

    def configure(self):
        # This package is consumed exclusively through its C ABI, so the C++
        # standard/stdlib used to build it is not part of its interface.
        #
        # More importantly, it must NOT inherit the consumer's C++20: upstream
        # pins CMAKE_CXX_STANDARD 17 and its vendored dependencies do not
        # compile as C++20 (simple-sentencepiece uses std::result_of, removed
        # in C++20). Dropping the setting stops CMakeToolchain from emitting
        # CMAKE_CXX_STANDARD, letting upstream choose 17, and keeps the
        # package id independent of the consumer's cppstd.
        self.settings.rm_safe("compiler.cppstd")
        self.settings.rm_safe("compiler.libcxx")

    def validate(self):
        # sherpa-onnx's Windows onnxruntime download logic (cmake/onnxruntime-win-x64.cmake)
        # hard-requires a Visual Studio generator (it keys off CMAKE_VS_PLATFORM_NAME)
        # and BUILD_SHARED_LIBS=ON, which is what we use here.
        if self.settings.os == "Windows" and not self.options.shared:
            raise ConanInvalidConfiguration(
                "sherpa-onnx on Windows is only packaged as a shared library by this recipe; "
                "upstream's onnxruntime-win-x64.cmake fails with BUILD_SHARED_LIBS=OFF."
            )

    def source(self):
        get(self,
            f"https://github.com/k2-fsa/sherpa-onnx/archive/refs/tags/v{self.version}.tar.gz",
            strip_root=True)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["SHERPA_ONNX_ENABLE_PYTHON"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_TESTS"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_CHECK"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_PORTAUDIO"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_WEBSOCKET"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_JNI"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_C_API"] = True
        tc.cache_variables["SHERPA_ONNX_ENABLE_BINARY"] = False
        tc.cache_variables["SHERPA_ONNX_BUILD_C_API_EXAMPLES"] = False
        tc.cache_variables["SHERPA_ONNX_ENABLE_TTS"] = bool(self.options.tts)
        tc.cache_variables["SHERPA_ONNX_ENABLE_GPU"] = bool(self.options.cuda)
        tc.cache_variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        # Always fetch the pinned onnxruntime upstream declares instead of
        # silently picking up whatever happens to be installed on the machine.
        tc.cache_variables["SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE"] = False

        if self.settings.os == "Windows":
            # Upstream defaults SHERPA_ONNX_USE_STATIC_CRT=ON (/MT). Conan's
            # profile here uses the dynamic CRT (/MD), and the flag also selects
            # which prebuilt onnxruntime archive gets downloaded, so it must
            # follow compiler.runtime.
            static_crt = str(self.settings.compiler.runtime) == "static"
            tc.cache_variables["SHERPA_ONNX_USE_STATIC_CRT"] = static_crt
            # cmake/onnxruntime-win-x64.cmake builds the onnxruntime archive
            # name from CMAKE_BUILD_TYPE and hard-errors when it is empty.
            # CMakeToolchain leaves it unset for multi-config generators (the
            # Visual Studio generator is required here -- see validate()), so
            # set it explicitly.
            tc.cache_variables["CMAKE_BUILD_TYPE"] = str(self.settings.build_type)
            # openfst (pulled in by kaldifst) uses M_LN2 from <cmath>, which
            # the MSVC CRT only exposes when _USE_MATH_DEFINES is defined.
            # Upstream's own CMake only special-cases MinGW.
            tc.preprocessor_definitions["_USE_MATH_DEFINES"] = 1
        tc.generate()

    def build(self):
        cmake = CMake(self)
        for attempt in range(1, self._configure_attempts + 1):
            try:
                cmake.configure()
                break
            except ConanException:
                if attempt == self._configure_attempts:
                    raise
                self.output.warning(
                    f"cmake.configure() failed (attempt {attempt}/{self._configure_attempts}); "
                    "retrying -- this is usually a flaky FetchContent download.")
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

        copy(self, "LICENSE", self.source_folder,
             os.path.join(self.package_folder, "licenses"))

        # Upstream's `install(TARGETS ... DESTINATION lib)` sends the Windows
        # import libs AND the DLLs to lib/. Conan consumers expect runtime
        # artifacts in bin/, so mirror them there and drop the copies in lib/.
        if self.settings.os == "Windows":
            lib_dir = os.path.join(self.package_folder, "lib")
            copy(self, "*.dll", lib_dir,
                 os.path.join(self.package_folder, "bin"), keep_path=False)
            copy(self, "*.dll", os.path.join(self.build_folder, "bin"),
                 os.path.join(self.package_folder, "bin"), keep_path=False)
            rm(self, "*.dll", lib_dir)

        # Upstream drops a pkg-config file at the package root. Conan consumers
        # go through CMakeDeps, and its paths point into the build tree, so it
        # would only be misleading.
        rm(self, "sherpa-onnx.pc", self.package_folder)

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "sherpa-onnx")
        self.cpp_info.set_property("cmake_target_name", "sherpa-onnx::sherpa-onnx")

        # Only the C API is part of this package's supported surface. The C++
        # convenience wrapper (sherpa-onnx-cxx-api) is installed too, but it is
        # not ABI-stable across compilers, so we do not advertise it.
        self.cpp_info.libs = ["sherpa-onnx-c-api"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libdirs = ["lib"]
        self.cpp_info.bindirs = ["bin"]

        if self.options.shared:
            # c-api.h picks __declspec(dllimport) only when this is defined and
            # SHERPA_ONNX_BUILD_MAIN_LIB is not.
            self.cpp_info.defines = ["SHERPA_ONNX_BUILD_SHARED_LIBS=1"]

        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread", "dl", "m"]
