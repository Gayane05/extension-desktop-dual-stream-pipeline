// desktop/src/core/secret_store.cpp
//
// DPAPI implementation of the secret_store.h seam. CryptProtectData derives
// the encryption key from the current Windows user's credentials, so no key
// material is stored by this app at all. Base64 transport encoding comes
// from the same crypt32 library, keeping this file dependency-free beyond
// what the target already links.
#include "core/secret_store.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <wincrypt.h>

#include <vector>

namespace dsp
{

namespace
{

// Application-specific additional entropy mixed into the DPAPI key
// derivation. This is not a secret (it ships in the binary); it only stops
// OTHER applications' casual CryptUnprotectData calls from decoding our blob
// by accident. Changing it invalidates every previously saved secret.
constexpr BYTE kAppEntropy[] = "dual-stream-transcriber.secret-store.v1";

DATA_BLOB appEntropyBlob()
{
    DATA_BLOB entropy;
    entropy.pbData = const_cast<BYTE*>(kAppEntropy);
    entropy.cbData = sizeof(kAppEntropy);
    return entropy;
}

// Base64-encodes `bytes` without line breaks. Returns nullopt on failure.
std::optional<std::string> toBase64(const BYTE* bytes, DWORD byteCount)
{
    DWORD encodedLength = 0;
    if (!CryptBinaryToStringA(bytes, byteCount, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                              &encodedLength))
    {
        return std::nullopt;
    }
    std::string encoded(encodedLength, '\0');
    if (!CryptBinaryToStringA(bytes, byteCount, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              encoded.data(), &encodedLength))
    {
        return std::nullopt;
    }
    // CryptBinaryToStringA's returned length excludes the NUL it also wrote;
    // trim the string to the real payload.
    encoded.resize(encodedLength);
    return encoded;
}

// Decodes base64 text into bytes. Returns nullopt when the text is not
// valid base64.
std::optional<std::vector<BYTE>> fromBase64(const std::string& encoded)
{
    if (encoded.empty())
    {
        return std::nullopt;
    }
    DWORD decodedLength = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &decodedLength,
                              nullptr, nullptr))
    {
        return std::nullopt;
    }
    std::vector<BYTE> decoded(decodedLength);
    if (!CryptStringToBinaryA(encoded.c_str(), 0, CRYPT_STRING_BASE64, decoded.data(),
                              &decodedLength, nullptr, nullptr))
    {
        return std::nullopt;
    }
    decoded.resize(decodedLength);
    return decoded;
}

}  // namespace

std::optional<std::string> protectSecret(const std::string& plaintext)
{
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());
    DATA_BLOB entropy = appEntropyBlob();
    DATA_BLOB output{};
    // CRYPTPROTECT_UI_FORBIDDEN: never pop a system dialog -- this runs from
    // settings saves and headless-adjacent code paths.
    if (!CryptProtectData(&input, L"Deepgram API key", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
    {
        return std::nullopt;
    }
    auto encoded = toBase64(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return encoded;
}

std::optional<std::string> unprotectSecret(const std::string& base64Blob)
{
    auto decoded = fromBase64(base64Blob);
    if (!decoded)
    {
        return std::nullopt;
    }
    DATA_BLOB input;
    input.pbData = decoded->data();
    input.cbData = static_cast<DWORD>(decoded->size());
    DATA_BLOB entropy = appEntropyBlob();
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
    {
        return std::nullopt;
    }
    std::string plaintext(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

}  // namespace dsp

#endif  // _WIN32
