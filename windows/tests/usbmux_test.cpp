#include <gtest/gtest.h>

#include "tethercam/formats.h"
#include "tethercam/plist.h"
#include "tethercam/usbmux.h"

using namespace tethercam;

namespace {

// Real ListDevices reply captured from Apple Devices 1.1540 on Windows 11 (identifiers shortened).
constexpr const char* kListReply = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>DeviceList</key>
	<array>
		<dict>
			<key>DeviceID</key>
			<integer>14</integer>
			<key>MessageType</key>
			<string>Attached</string>
			<key>Properties</key>
			<dict>
				<key>ConnectionType</key>
				<string>Network</string>
				<key>DeviceID</key>
				<integer>14</integer>
				<key>InterfaceIndex</key>
				<integer>6</integer>
				<key>NetworkAddress</key>
				<data>
				FwAAAAAAAAAkBHwAAEHuPwTNKnDp
				</data>
				<key>SerialNumber</key>
				<string>00008130-0000000000000000</string>
			</dict>
		</dict>
		<dict>
			<key>DeviceID</key>
			<integer>3</integer>
			<key>MessageType</key>
			<string>Attached</string>
			<key>Properties</key>
			<dict>
				<key>ConnectionType</key>
				<string>USB</string>
				<key>DeviceID</key>
				<integer>3</integer>
				<key>ProductID</key>
				<integer>4776</integer>
				<key>SerialNumber</key>
				<string>abc&amp;def</string>
			</dict>
		</dict>
	</array>
</dict>
</plist>
)";

}  // namespace

TEST(Plist, ParsesUsbmuxdDeviceList) {
    auto v = plist::FromXml(kListReply);
    ASSERT_TRUE(v);
    auto devices = usbmux::ParseDeviceList(*v);
    ASSERT_TRUE(devices);
    ASSERT_EQ(devices->size(), 2u);
    EXPECT_EQ((*devices)[0].device_id, 14u);
    EXPECT_EQ((*devices)[0].connection_type, "Network");
    EXPECT_EQ((*devices)[1].device_id, 3u);
    EXPECT_EQ((*devices)[1].connection_type, "USB");
    EXPECT_EQ((*devices)[1].product_id, 4776u);
    EXPECT_EQ((*devices)[1].serial, "abc&def");
}

TEST(Plist, RoundTripsAllTypes) {
    plist::Dict d;
    d["s"] = "a<b>&c";
    d["i"] = int64_t(-5);
    d["t"] = true;
    d["f"] = false;
    d["d"] = plist::Data{{0, 1, 2, 250, 251}};
    d["a"] = plist::Array{plist::Value(1), plist::Value("x")};
    d["e"] = plist::Dict{};
    auto xml = plist::ToXml(plist::Value(d));
    auto back = plist::FromXml(xml);
    ASSERT_TRUE(back);
    EXPECT_EQ(*back->Get("s")->AsString(), "a<b>&c");
    EXPECT_EQ(*back->Get("i")->AsInt(), -5);
    EXPECT_EQ(std::get<plist::Data>(back->Get("d")->v).bytes, (std::vector<uint8_t>{0, 1, 2, 250, 251}));
    EXPECT_EQ(back->Get("a")->AsArray()->size(), 2u);
    EXPECT_TRUE(back->Get("e")->AsDict()->empty());
}

TEST(Plist, RejectsMalformed) {
    EXPECT_FALSE(plist::FromXml("<plist><dict><key>a</key>"));
    EXPECT_FALSE(plist::FromXml("not xml"));
    std::string deep = "<plist>";
    for (int i = 0; i < 100; ++i) deep += "<array>";
    EXPECT_FALSE(plist::FromXml(deep));
}

TEST(Usbmux, PortIsByteSwapped) {
    EXPECT_EQ(usbmux::SwapPort(5321), 0xC914);
    EXPECT_EQ(usbmux::SwapPort(62078), 0x7EF2);
}

TEST(Usbmux, PacketHeader) {
    plist::Dict d;
    d["MessageType"] = "ListDevices";
    auto pkt = usbmux::EncodePacket(plist::Value(d), 7);
    ASSERT_GT(pkt.size(), 16u);
    EXPECT_EQ(pkt[0] | pkt[1] << 8 | pkt[2] << 16 | pkt[3] << 24, int(pkt.size()));
    EXPECT_EQ(pkt[4], 1);   // version: plist
    EXPECT_EQ(pkt[8], 8);   // message: plist
    EXPECT_EQ(pkt[12], 7);  // tag
    auto body = usbmux::DecodeBody({pkt.data() + 16, pkt.size() - 16});
    ASSERT_TRUE(body);
    EXPECT_EQ(*body->Get("MessageType")->AsString(), "ListDevices");
}

TEST(Usbmux, ParsesEvents) {
    plist::Dict det;
    det["MessageType"] = "Detached";
    det["DeviceID"] = 9;
    auto ev = usbmux::ParseEvent(plist::Value(det));
    ASSERT_TRUE(ev);
    EXPECT_EQ(ev->kind, usbmux::Event::Kind::Detached);
    EXPECT_EQ(ev->device.device_id, 9u);
}

TEST(Formats, ChoosesExactThenCoveringThenLargest) {
    std::vector<proto::Format> caps = {{1280, 720, 60}, {1920, 1080, 60}, {3840, 2160, 30}};
    MediaFormat out;
    ASSERT_TRUE(ChooseDeviceFormat(caps, {1920, 1080, 30}, out));
    EXPECT_EQ(out, (MediaFormat{1920, 1080, 30}));
    ASSERT_TRUE(ChooseDeviceFormat(caps, {2560, 1440, 60}, out));
    EXPECT_EQ(out, (MediaFormat{3840, 2160, 30}));  // covering format, fps clamped
    ASSERT_TRUE(ChooseDeviceFormat(caps, {640, 480, 30}, out));
    EXPECT_EQ(out, (MediaFormat{1280, 720, 30}));
    std::vector<proto::Format> small = {{1280, 720, 30}};
    ASSERT_TRUE(ChooseDeviceFormat(small, {3840, 2160, 60}, out));
    EXPECT_EQ(out, (MediaFormat{1280, 720, 30}));
    EXPECT_FALSE(ChooseDeviceFormat({}, {1920, 1080, 30}, out));
}

TEST(Formats, BitrateTableAnchors) {
    EXPECT_NEAR(DefaultBitrateKbps(1920, 1080, 30), 12000, 100);
    EXPECT_NEAR(DefaultBitrateKbps(3840, 2160, 30), 40000, 2500);
    EXPECT_GT(DefaultBitrateKbps(3840, 2160, 60), 60000u);
    EXPECT_LE(DefaultBitrateKbps(3840, 2160, 60), 80000u);
}
