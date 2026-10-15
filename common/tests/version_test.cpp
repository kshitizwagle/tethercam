#include <gtest/gtest.h>

#include "tethercam/version.h"

// CI stamps the version per build (TETHERCAM_BUILD_VERSION), so compare with what CMake configured.
TEST(Version, MatchesProject) {
    EXPECT_EQ(tethercam::Version(), TETHERCAM_EXPECTED_VERSION);
    EXPECT_FALSE(tethercam::Version().empty());
}
