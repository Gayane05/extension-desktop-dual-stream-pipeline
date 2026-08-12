# desktop/conanfile.py
from conan import ConanFile
from conan.tools.cmake import cmake_layout


class TranscriberConan(ConanFile):
    name = "dual-stream-transcriber"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("ixwebsocket/11.4.5")
        self.requires("rapidjson/cci.20230929")
        self.requires("imgui/1.90.9")
        self.requires("gtest/1.15.0")
        # sherpa-onnx added in Task 7 via local recipe

    def configure(self):
        # TLS needed for the Deepgram WSS client. `tls` is not a bool on this
        # recipe revision; valid values are mbedtls/openssl/applessl/False.
        # Verify with `conan graph info .` if this changes upstream.
        self.options["ixwebsocket"].tls = "openssl"

    def layout(self):
        cmake_layout(self)
