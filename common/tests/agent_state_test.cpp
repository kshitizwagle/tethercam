#include <gtest/gtest.h>

#include "tethercam/agent_state.h"
#include "tethercam/version.h"

using namespace tethercam;

// The status API is a stable contract (docs/STATUS_API.md): field names, order and state names.
TEST(StatusJson, IdleMatchesContract) {
    agent::Snapshot s;
    EXPECT_EQ(agent::StatusJson(s),
              std::string(R"({"state":"no_iphone","live":false,"device":"","camera":"","lens":"","width":0,"height":0,)"
                          R"("fps":0,"mirror":false,"rotation":0,"detail":"","thermal":-1,"battery":-1,"charging":false,)"
                          R"("version":")") + std::string(Version()) + R"(","quality":"Auto","transport":""})");
}

TEST(StatusJson, LiveWithSelectedLens) {
    agent::Snapshot s;
    s.status = AgentStatus::Streaming;
    s.device_name = "Kshitiz's \"iPhone\"";
    proto::Camera cam;
    cam.camera_id = 2;
    cam.position = proto::Position::Back;
    cam.lens = proto::Lens::UltraWide;
    cam.label = "Back Ultra Wide";
    s.cameras.push_back(cam);
    s.selected_camera = 2;
    s.width = 3840;
    s.height = 2160;
    s.fps = 30;
    s.quality = proto::Quality::UHD30;
    const std::string j = agent::StatusJson(s);
    EXPECT_NE(j.find(R"("state":"live","live":true)"), std::string::npos) << j;
    EXPECT_NE(j.find(R"("device":"Kshitiz's \"iPhone\"")"), std::string::npos) << j;
    EXPECT_NE(j.find(R"("camera":"Back Ultra Wide","lens":"ultra_wide")"), std::string::npos) << j;
    EXPECT_NE(j.find(R"("quality":"4K / 30 fps")"), std::string::npos) << j;
    s.transport = "wifi";
    EXPECT_NE(agent::StatusJson(s).find(R"("transport":"wifi")"), std::string::npos);
}

TEST(StatusJson, StreamingWithoutFramesIsNotLive) {
    agent::Snapshot s;
    s.status = AgentStatus::Streaming;
    EXPECT_NE(agent::StatusJson(s).find(R"("live":false)"), std::string::npos);
}
