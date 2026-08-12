// desktop/tests/test_sherpa_link.cpp
//
// Link/load smoke test for the vendored sherpa-onnx Conan package. It does not
// exercise recognition (that needs model files); it only proves that the header
// is on the include path, that the import library resolves at link time, and
// that sherpa-onnx-c-api.dll + onnxruntime.dll are loadable next to the test
// binary at runtime.
#include <gtest/gtest.h>
#include <sherpa-onnx/c-api/c-api.h>

TEST(SherpaLink, RejectsEmptyConfigGracefully) {
    SherpaOnnxOnlineRecognizerConfig config{};
    // Invalid (empty model paths) -- must return null, not crash. Proves we
    // compiled against the header and linked/loaded the library.
    const SherpaOnnxOnlineRecognizer* rec = SherpaOnnxCreateOnlineRecognizer(&config);
    EXPECT_EQ(rec, nullptr);
    // EXPECT_ rather than ASSERT_ above, so clean up if a future sherpa-onnx
    // ever does return a recognizer here instead of leaking it.
    SherpaOnnxDestroyOnlineRecognizer(rec);
}
