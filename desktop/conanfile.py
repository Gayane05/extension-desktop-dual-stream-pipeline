# desktop/conanfile.py
from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
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

        # This project targets C++20, and gtest/1.15.0 requires C++17+. A
        # freshly auto-detected Conan profile commonly leaves compiler.cppstd
        # at an older default (e.g. 14 for MSVC). Checking this in validate()
        # is too late here: Conan builds/downloads dependency binaries before
        # it gets to validating the consumer recipe, so a stale cppstd would
        # otherwise surface as a confusing native compiler error deep into a
        # `--build=missing` source build of gtest. Check it here in
        # configure(), which runs during graph computation before any
        # dependency binary is resolved or built, so this fails immediately
        # with a clear message instead.
        cppstd = self.settings.get_safe("compiler.cppstd")
        cppstd_num = self._cppstd_as_int(cppstd)
        if cppstd_num is None or cppstd_num < 20:
            raise ConanInvalidConfiguration(
                f"compiler.cppstd={cppstd} but this project requires C++20. "
                "Use --profile:all=conan_profiles/default (see desktop/conan_profiles/default), "
                "or pass -s compiler.cppstd=20 explicitly."
            )

    @staticmethod
    def _cppstd_as_int(cppstd):
        # compiler.cppstd values include GNU-dialect strings (e.g. "gnu17",
        # "gnu20") on GCC/Clang profiles, not just plain numbers like "20"
        # (MSVC). Strip a leading "gnu" prefix before parsing so this works
        # on Windows/macOS/Linux alike, and never raise on unparseable input
        # -- treat it as "unknown/too old" instead of crashing.
        if cppstd is None:
            return None
        text = str(cppstd)
        if text.startswith("gnu"):
            text = text[len("gnu"):]
        try:
            return int(text)
        except ValueError:
            return None

    def layout(self):
        cmake_layout(self)
