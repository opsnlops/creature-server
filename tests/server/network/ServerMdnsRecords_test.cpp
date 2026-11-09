#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "server/config.h"
#include "server/network/ServerMdnsRecords.h"

namespace creatures::network {

namespace {

using ::testing::Contains;
using ::testing::Key;
using ::testing::Not;
using ::testing::Pair;

ServerDetails mainstageDetails() {
    ServerDetails details;
    details.name = "Fuzzball";
    details.version = "3.51.8";
    details.port = 8000;
    details.travelMode = false;
    details.rtpAudio = true;
    details.audioChannels = 17;
    details.audioSampleRate = 48000;
    details.soundLibrary = "/mnt/creatures/sounds";
    details.lipSyncEngine = "whisper";
    details.networkInterface = 2;
    return details;
}

} // namespace

TEST(ServerMdnsRecords, DescribesTheServer) {
    const auto service = makeServerService(mainstageDetails());

    EXPECT_EQ(service.name, "fuzzball");
    EXPECT_EQ(service.type, "_creature-server._tcp");
    EXPECT_EQ(service.port, 8000);

    EXPECT_THAT(service.txt, Contains(Pair("txtvers", "1")));
    EXPECT_THAT(service.txt, Contains(Pair("version", "3.51.8")));
    EXPECT_THAT(service.txt, Contains(Pair("api", "/api/v1")));
    EXPECT_THAT(service.txt, Contains(Pair("mode", "mainstage")));
    EXPECT_THAT(service.txt, Contains(Pair("audio", "rtp")));
    EXPECT_THAT(service.txt, Contains(Pair("audio_channels", "17")));
    EXPECT_THAT(service.txt, Contains(Pair("audio_rate_hz", "48000")));
    EXPECT_THAT(service.txt, Contains(Pair("sound_library", "/mnt/creatures/sounds")));
    EXPECT_THAT(service.txt, Contains(Pair("lipsync", "whisper")));
    EXPECT_THAT(service.txt, Contains(Pair("network_interface", "2")));
}

TEST(ServerMdnsRecords, TxtversComesFirst) {
    const auto service = makeServerService(mainstageDetails());
    ASSERT_FALSE(service.txt.empty());
    EXPECT_EQ(service.txt.front().first, "txtvers");
}

TEST(ServerMdnsRecords, RtpModeAdvertisesThePortsAndNotTheDevice) {
    const auto service = makeServerService(mainstageDetails());

    EXPECT_THAT(service.txt, Contains(Pair("rtp_port", std::to_string(creatures::rtp::RTP_PORT))));
    EXPECT_THAT(service.txt, Contains(Pair("rtcp_port", std::to_string(creatures::rtp::RTCP_PORT))));
    EXPECT_THAT(service.txt, Not(Contains(Key("audio_device"))));
}

TEST(ServerMdnsRecords, LocalModeAdvertisesTheDeviceAndNotThePorts) {
    auto details = mainstageDetails();
    details.rtpAudio = false;
    details.audioDevice = "plughw:CARD=BossDAC,DEV=0";

    const auto service = makeServerService(details);

    EXPECT_THAT(service.txt, Contains(Pair("audio", "local")));
    EXPECT_THAT(service.txt, Contains(Pair("audio_device", "plughw:CARD=BossDAC,DEV=0")));
    EXPECT_THAT(service.txt, Not(Contains(Key("rtp_port"))));
    EXPECT_THAT(service.txt, Not(Contains(Key("rtcp_port"))));
}

TEST(ServerMdnsRecords, AnUnsetLocalDeviceSaysDefaultRatherThanNothing) {
    auto details = mainstageDetails();
    details.rtpAudio = false;
    details.audioDevice = "";

    EXPECT_THAT(makeServerService(details).txt, Contains(Pair("audio_device", "default")));
}

TEST(ServerMdnsRecords, TravelModeSaysSo) {
    auto details = mainstageDetails();
    details.travelMode = true;
    details.rtpAudio = false;
    details.audioChannels = 2;

    const auto service = makeServerService(details);

    EXPECT_THAT(service.txt, Contains(Pair("mode", "travel")));
    EXPECT_THAT(service.txt, Contains(Pair("audio_channels", "2")));
}

TEST(ServerMdnsRecords, FitsInOnePacket) {
    auto service = makeServerService(mainstageDetails());
    EXPECT_LE(fitTxtRecord(service), MDNS_TXT_RECORD_TARGET_BYTES);
}

TEST(ServerMdnsRecords, NoValueIsEmpty) {
    const auto service = makeServerService(mainstageDetails());
    for (const auto &[key, value] : service.txt) {
        EXPECT_FALSE(key.empty());
        EXPECT_FALSE(value.empty()) << "TXT key '" << key << "' has no value";
    }
}

TEST(ServerMdnsRecords, DetailsComeFromTheConfiguration) {
    const Configuration config;
    const auto details = serverDetailsFrom(config, "Fuzzball", "3.51.8", 8123);

    EXPECT_EQ(details.name, "Fuzzball");
    EXPECT_EQ(details.version, "3.51.8");
    EXPECT_EQ(details.port, 8123);

    // Whatever the defaults are, they must be the ones we advertise
    EXPECT_EQ(details.travelMode, config.getTravelMode());
    EXPECT_EQ(details.rtpAudio, config.getAudioMode() == Configuration::AudioMode::RTP);
    EXPECT_EQ(details.audioChannels, config.getSoundChannels());
    EXPECT_EQ(details.audioSampleRate, config.getSoundFrequency());
    EXPECT_EQ(details.soundLibrary, config.getSoundFileLocation());
    EXPECT_EQ(details.lipSyncEngine, config.getLipSyncEngine());
    EXPECT_EQ(details.networkInterface, config.getNetworkDevice());
}

} // namespace creatures::network
