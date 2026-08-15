// desktop/src/core/secret_store.h
//
// At-rest protection for small secrets (currently the Deepgram API key in
// settings.json). The implementation uses Windows DPAPI bound to the current
// Windows user: the returned blob decrypts only under this user's account on
// this machine, which protects against other local users, stolen disks, and
// copied/backed-up app folders. It does NOT protect against code already
// running as the same user -- no local storage scheme does.
//
// This pair of functions is the deliberate seam for a stronger backend: a
// TPM-backed implementation (CNG Platform Crypto Provider wrapping the
// secret with a chip-resident key) can replace the .cpp without touching any
// caller.
#pragma once
#include <optional>
#include <string>

namespace dsp
{

// Encrypts `plaintext` for the current Windows user and returns a base64
// blob suitable for storing in a text file. Returns nullopt on failure.
std::optional<std::string> protectSecret(const std::string& plaintext);

// Inverse of protectSecret. Returns nullopt when the blob is corrupted, was
// produced by a different user/machine, or is not a valid protected blob --
// callers should treat that as "no secret stored" and ask the user again.
std::optional<std::string> unprotectSecret(const std::string& base64Blob);

}  // namespace dsp
