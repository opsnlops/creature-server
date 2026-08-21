
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/audio/Mp3Writer.h"

namespace {

using creatures::audio::encodeMonoToMp3;
using creatures::audio::kShareableSampleRate;

std::vector<int16_t> makeSine(double frequencyHz, double seconds) {
    const auto sampleCount = static_cast<size_t>(seconds * kShareableSampleRate);
    std::vector<int16_t> samples(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
        const double t = static_cast<double>(i) / kShareableSampleRate;
        samples[i] = static_cast<int16_t>(12000.0 * std::sin(2.0 * M_PI * frequencyHz * t));
    }
    return samples;
}

// A bare MP3 stream starts with an MPEG frame sync (11 set bits: 0xFF followed by 0xEx/0xFx);
// with an ID3v2 tag prepended it starts with "ID3". Accept either as "looks like an MP3".
bool looksLikeMp3(const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 3) {
        return false;
    }
    if (std::memcmp(bytes.data(), "ID3", 3) == 0) {
        return true;
    }
    return bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0;
}

TEST(Mp3Writer, EncodesASineWaveToAValidLookingMp3File) {
    const auto samples = makeSine(440.0, 1.0);
    auto result = encodeMonoToMp3(samples, kShareableSampleRate);
    ASSERT_TRUE(result.isSuccess());

    const auto bytes = result.getValue().value();
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_TRUE(looksLikeMp3(bytes));

    // The whole point: dramatically smaller than the raw PCM.
    EXPECT_LT(bytes.size(), samples.size() * sizeof(int16_t) / 4);
}

TEST(Mp3Writer, OutputIsDeterministicForTheSameInput) {
    const auto samples = makeSine(220.0, 0.25);
    auto first = encodeMonoToMp3(samples, kShareableSampleRate);
    auto second = encodeMonoToMp3(samples, kShareableSampleRate);
    ASSERT_TRUE(first.isSuccess());
    ASSERT_TRUE(second.isSuccess());
    EXPECT_EQ(first.getValue().value(), second.getValue().value());
}

TEST(Mp3Writer, RejectsNon48kSampleRates) {
    const auto samples = makeSine(440.0, 0.1);
    auto result = encodeMonoToMp3(samples, 44100);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(creatures::ServerError::InvalidData, result.getError().value().getCode());
}

TEST(Mp3Writer, RejectsEmptyInput) {
    auto result = encodeMonoToMp3({}, kShareableSampleRate);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(creatures::ServerError::InvalidData, result.getError().value().getCode());
}

// Decode an ID3v2.4 syncsafe (7-bits-per-byte) size.
uint32_t readSyncsafe32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 21) | (static_cast<uint32_t>(p[1]) << 14) |
           (static_cast<uint32_t>(p[2]) << 7) | static_cast<uint32_t>(p[3]);
}

// Walk the ID3v2 tag at the front of an MP3 and return each frame as (frame id, body).
// Fails the test (returning what it has) if the header is missing or a frame overruns.
std::vector<std::pair<std::string, std::string>> parseId3Frames(const std::vector<uint8_t> &bytes) {
    std::vector<std::pair<std::string, std::string>> frames;
    if (bytes.size() < 10 || std::memcmp(bytes.data(), "ID3", 3) != 0) {
        ADD_FAILURE() << "no ID3v2 header at the front of the stream";
        return frames;
    }
    EXPECT_EQ(0x04, bytes[3]) << "expected an ID3v2.4 tag";
    const uint32_t tagSize = readSyncsafe32(bytes.data() + 6);
    if (10u + tagSize > bytes.size()) {
        ADD_FAILURE() << "ID3v2 tag size overruns the stream";
        return frames;
    }
    size_t pos = 10;
    const size_t end = 10 + tagSize;
    while (pos + 10 <= end) {
        const std::string frameId(bytes.begin() + pos, bytes.begin() + pos + 4);
        const uint32_t frameSize = readSyncsafe32(bytes.data() + pos + 4);
        pos += 10;
        if (pos + frameSize > end) {
            ADD_FAILURE() << "frame " << frameId << " overruns the tag";
            return frames;
        }
        frames.emplace_back(frameId, std::string(bytes.begin() + pos, bytes.begin() + pos + frameSize));
        pos += frameSize;
    }
    return frames;
}

std::string frameBody(const std::vector<std::pair<std::string, std::string>> &frames, const std::string &frameId) {
    for (const auto &[id, body] : frames) {
        if (id == frameId) {
            return body;
        }
    }
    return {};
}

// The ID3v2.4 text encoding byte for UTF-8, kept as its own literal so it can't swallow
// the first characters of the value it's concatenated with (hex escapes are greedy).
constexpr const char kUtf8[] = "\x03";

TEST(Mp3Writer, EmbedsProvenanceInId3v2Frames) {
    const auto samples = makeSine(440.0, 0.1);
    creatures::audio::Id3Comments comments = {
        {"TITLE", "Web Scale"}, {"SOURCE_SCRIPT_ID", "script-9"}, {"DESCRIPTION", "Beaky: hi\nPip: bye"}};
    auto result = encodeMonoToMp3(samples, kShareableSampleRate, creatures::audio::kShareableMp3Bitrate, comments);
    ASSERT_TRUE(result.isSuccess());
    const auto bytes = result.getValue().value();

    const auto frames = parseId3Frames(bytes);
    // TITLE lands in the standard title frame (0x03 = UTF-8 encoding byte).
    EXPECT_EQ(std::string(kUtf8) + "Web Scale", frameBody(frames, "TIT2"));
    // DESCRIPTION becomes a COMM frame: encoding, "eng", empty short description, text.
    EXPECT_EQ(std::string(kUtf8) + "eng" + '\0' + "Beaky: hi\nPip: bye", frameBody(frames, "COMM"));
    // Non-display provenance stays in a TXXX frame: key, terminator, value.
    EXPECT_EQ(std::string(kUtf8) + "SOURCE_SCRIPT_ID" + '\0' + "script-9", frameBody(frames, "TXXX"));
}

TEST(Mp3Writer, MapsDisplayKeysToStandardId3Frames) {
    const auto samples = makeSine(440.0, 0.1);
    creatures::audio::Id3Comments comments = {{"ARTIST", "Beaky, Pip"},
                                              {"ALBUMARTIST", "April's Creature Workshop"},
                                              {"ALBUM", "April's Creature Workshop"},
                                              {"GENRE", "Spoken Word"},
                                              {"PUBLISHER", "April's Creature Workshop"},
                                              {"COMPOSER", "ElevenLabs Music"},
                                              {"LYRICS", "Beaky: hi\nPip: bye"}};
    auto result = encodeMonoToMp3(samples, kShareableSampleRate, creatures::audio::kShareableMp3Bitrate, comments);
    ASSERT_TRUE(result.isSuccess());

    const auto frames = parseId3Frames(result.getValue().value());
    EXPECT_EQ(std::string(kUtf8) + "Beaky, Pip", frameBody(frames, "TPE1"));
    EXPECT_EQ(std::string(kUtf8) + "April's Creature Workshop", frameBody(frames, "TPE2"));
    EXPECT_EQ(std::string(kUtf8) + "April's Creature Workshop", frameBody(frames, "TALB"));
    EXPECT_EQ(std::string(kUtf8) + "Spoken Word", frameBody(frames, "TCON"));
    EXPECT_EQ(std::string(kUtf8) + "April's Creature Workshop", frameBody(frames, "TPUB"));
    EXPECT_EQ(std::string(kUtf8) + "ElevenLabs Music", frameBody(frames, "TCOM"));
    // The dialog transcript rides in USLT so lyrics panes can show it.
    EXPECT_EQ(std::string(kUtf8) + "eng" + '\0' + "Beaky: hi\nPip: bye", frameBody(frames, "USLT"));
}

TEST(Mp3Writer, AddsEncoderSettingsAndExactDurationOnItsOwn) {
    // 0.1s at 48kHz is 4800 samples, exactly 100 ms.
    const auto samples = makeSine(440.0, 0.1);
    creatures::audio::Id3Comments comments = {{"TITLE", "Beep"}};
    auto result = encodeMonoToMp3(samples, kShareableSampleRate, creatures::audio::kShareableMp3Bitrate, comments);
    ASSERT_TRUE(result.isSuccess());

    const auto frames = parseId3Frames(result.getValue().value());
    EXPECT_EQ(std::string(kUtf8) + "100", frameBody(frames, "TLEN"));
    const auto encoder = frameBody(frames, "TSSE");
    EXPECT_NE(std::string::npos, encoder.find("LAME"));
    EXPECT_NE(std::string::npos, encoder.find("128 kbps CBR mono"));
}

TEST(Mp3Writer, HandlesInputShorterThanOneFrame) {
    // 5ms of audio — the encoder should still flush a valid stream.
    const auto samples = makeSine(440.0, 0.005);
    auto result = encodeMonoToMp3(samples, kShareableSampleRate);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(looksLikeMp3(result.getValue().value()));
}

} // namespace
