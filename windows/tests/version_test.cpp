#include <gtest/gtest.h>

#include "tethercam/version.h"

TEST(Version, MatchesProject) { EXPECT_EQ(tethercam::Version(), "0.1.0"); }
