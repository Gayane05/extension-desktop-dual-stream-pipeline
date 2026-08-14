// desktop/tests/test_smoke.cpp
//
// Trivial build/link sanity check: proves the dsp_tests binary links against
// dsp_core and runs at all, independent of any real feature under test.
#include <gtest/gtest.h>

#include "core/version.h"

TEST(Smoke, VersionIsSet)
{
    EXPECT_STREQ(dsp::kAppVersion, "0.1.0");
}
