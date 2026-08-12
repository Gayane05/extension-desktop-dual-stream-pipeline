#include <gtest/gtest.h>
#include "core/version.h"

TEST(Smoke, VersionIsSet) {
    EXPECT_STREQ(dsp::kAppVersion, "0.1.0");
}
