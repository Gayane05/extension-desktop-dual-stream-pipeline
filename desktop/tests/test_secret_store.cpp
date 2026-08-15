// desktop/tests/test_secret_store.cpp
//
// Unit tests for core/secret_store.h's DPAPI-backed secret protection:
// round trip, tamper/garbage rejection, and empty-input handling. All run
// as the current Windows user, which is exactly the binding DPAPI uses.
#include <gtest/gtest.h>

#include "core/secret_store.h"

using namespace dsp;

TEST(SecretStore, RoundTripRestoresPlaintext)
{
    const std::string plaintext = "dg-test-api-key-1234567890";
    auto protectedBlob = protectSecret(plaintext);
    ASSERT_TRUE(protectedBlob.has_value());
    // The blob must be ciphertext, not a disguised copy of the input.
    EXPECT_EQ(protectedBlob->find(plaintext), std::string::npos);
    auto restored = unprotectSecret(*protectedBlob);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, plaintext);
}

TEST(SecretStore, GarbageBlobsAreRejected)
{
    // Not base64 at all.
    EXPECT_FALSE(unprotectSecret("!!!not-base64!!!").has_value());
    // Valid base64 ("hello"), but not a DPAPI blob.
    EXPECT_FALSE(unprotectSecret("aGVsbG8=").has_value());
    EXPECT_FALSE(unprotectSecret("").has_value());
}

TEST(SecretStore, EmptyPlaintextRoundTrips)
{
    auto protectedBlob = protectSecret("");
    ASSERT_TRUE(protectedBlob.has_value());
    auto restored = unprotectSecret(*protectedBlob);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->empty());
}
