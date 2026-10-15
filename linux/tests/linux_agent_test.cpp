#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>

#include "settings.h"
#include "v4l2_output.h"

using namespace tethercam::agent;
namespace fs = std::filesystem;

TEST(Settings, PersistsAcrossInstances) {
    const fs::path dir = fs::temp_directory_path() / ("tcam-settings-" + std::to_string(getpid()));
    const fs::path file = dir / "sub" / "settings";
    {
        Settings s(file);
        s.Set("Camera", "Back Camera");
        s.Set("Quality", "3");
        s.Set("Bad", "a\nb");
    }
    Settings again(file);
    EXPECT_EQ(again.Get("Camera"), "Back Camera");
    EXPECT_EQ(again.Get("Quality"), "3");
    EXPECT_EQ(again.Get("Bad"), "ab");
    EXPECT_EQ(again.Get("Missing"), "");
    fs::remove_all(dir);
}

TEST(Settings, EmptyPathStaysInMemory) {
    Settings s({});
    s.Set("Mirror", "1");
    EXPECT_EQ(s.Get("Mirror"), "1");
}

TEST(V4l2, CountsOtherOpenersOnly) {
    // This process holding the file open must not count; nobody else has it open.
    const fs::path file = fs::temp_directory_path() / ("tcam-open-" + std::to_string(getpid()));
    FILE* f = std::fopen(file.c_str(), "w");
    ASSERT_TRUE(f);
    EXPECT_EQ(CountOtherOpeners(file.string()), 0);
    std::fclose(f);
    fs::remove(file);
}
