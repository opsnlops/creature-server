#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/ElevenLabsHttp.h"
#include "server/voice/MusicClient.h"

TEST(ElevenLabsHttp, CapturesMusicResponseIdentifiersCaseInsensitively) {
    creatures::voice::elevenlabs_http::ResponseHeaders headers;
    std::string requestId = "X-ElevenLabs-Request-ID: request-123\r\n";
    std::string songId = "Song-ID: song-456\r\n";
    EXPECT_EQ(creatures::voice::elevenlabs_http::captureResponseHeader(requestId.data(), 1, requestId.size(), &headers),
              requestId.size());
    EXPECT_EQ(creatures::voice::elevenlabs_http::captureResponseHeader(songId.data(), 1, songId.size(), &headers),
              songId.size());
    EXPECT_EQ(headers.requestId, "request-123");
    EXPECT_EQ(headers.songId, "song-456");
}

TEST(MusicClient, ParsesDetailedMultipartPcmAndMetadata) {
    const std::string boundary = "music-boundary";
    const std::string json =
        R"({"composition_plan":{"chunks":[{"duration_ms":3000}]},"song_metadata":{"title":"BGM"}})";
    std::string body = "--" + boundary + "\r\nContent-Type: application/json\r\n\r\n" + json + "\r\n";
    body += "--" + boundary + "\r\nContent-Type: application/octet-stream\r\n\r\n";
    body.push_back('\x01');
    body.push_back('\x00');
    body.push_back('\x02');
    body.push_back('\x00');
    body += "\r\n--" + boundary + "--\r\n";

    const std::vector<uint8_t> bytes(body.begin(), body.end());
    auto result =
        creatures::voice::MusicClient::parseDetailedResponse(bytes, "multipart/mixed; boundary=\"music-boundary\"");
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage()
                                                          : std::string("unexpected parse failure"));
    const auto parsed = result.getValue().value();
    ASSERT_EQ(parsed.audioPcm.size(), 2u);
    EXPECT_EQ(parsed.audioPcm[0], 1);
    EXPECT_EQ(parsed.sourceChannels, 2);
    EXPECT_EQ(parsed.responseMetadata["song_metadata"]["title"], "BGM");
    EXPECT_EQ(parsed.compositionPlan["chunks"][0]["duration_ms"], 3000);
    EXPECT_EQ(parsed.songMetadata["title"], "BGM");
}

TEST(MusicClient, RejectsMultipartWithoutAudio) {
    const std::string body = "--x\r\nContent-Type: application/json\r\n\r\n{}\r\n--x--\r\n";
    auto result = creatures::voice::MusicClient::parseDetailedResponse(std::vector<uint8_t>(body.begin(), body.end()),
                                                                       "multipart/mixed; boundary=x");
    EXPECT_FALSE(result.isSuccess());
}
