
#include <algorithm>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <spdlog/spdlog.h>

#include "server/audio/OggOpusWriter.h"
#include "server/namespace-stuffs.h"

namespace creatures::audio {

namespace {

// 20 ms frames — the conventional size for Ogg Opus files.
constexpr int kFrameSamples = 960;

// Any fixed stream serial is legal for a standalone single-stream file, and a
// deterministic one keeps encoded output byte-stable for tests.
constexpr int kStreamSerial = 0x5EA50;

void appendLE16(std::vector<uint8_t> &v, uint16_t value) {
    v.push_back(static_cast<uint8_t>(value & 0xff));
    v.push_back(static_cast<uint8_t>(value >> 8));
}

void appendLE32(std::vector<uint8_t> &v, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        v.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
    }
}

// RFC 7845 §5.1 identification header, channel mapping family 0 (mono).
std::vector<uint8_t> buildOpusHead(uint16_t preSkip, uint32_t inputSampleRate) {
    std::vector<uint8_t> head;
    const std::string magic = "OpusHead";
    head.insert(head.end(), magic.begin(), magic.end());
    head.push_back(1); // version
    head.push_back(1); // channel count
    appendLE16(head, preSkip);
    appendLE32(head, inputSampleRate);
    appendLE16(head, 0); // output gain (Q7.8 dB)
    head.push_back(0);   // channel mapping family
    return head;
}

// RFC 7845 §5.2 comment header, with optional user comments written as the
// Vorbis-comment "KEY=VALUE" convention.
std::vector<uint8_t> buildOpusTags(const OggComments &comments) {
    std::vector<uint8_t> tags;
    const std::string magic = "OpusTags";
    tags.insert(tags.end(), magic.begin(), magic.end());
    const std::string vendor = "creature-server";
    appendLE32(tags, static_cast<uint32_t>(vendor.size()));
    tags.insert(tags.end(), vendor.begin(), vendor.end());
    appendLE32(tags, static_cast<uint32_t>(comments.size()));
    for (const auto &[key, value] : comments) {
        const std::string entry = key + "=" + value;
        appendLE32(tags, static_cast<uint32_t>(entry.size()));
        tags.insert(tags.end(), entry.begin(), entry.end());
    }
    return tags;
}

void appendPage(std::vector<uint8_t> &out, const ogg_page &page) {
    out.insert(out.end(), page.header, page.header + page.header_len);
    out.insert(out.end(), page.body, page.body + page.body_len);
}

struct OggStreamGuard {
    ogg_stream_state state{};
    bool initialized = false;
    ~OggStreamGuard() {
        if (initialized) {
            ogg_stream_clear(&state);
        }
    }
};

} // namespace

Result<std::vector<uint8_t>> encodeMonoToOggOpus(const std::vector<int16_t> &samples, int sampleRate, int bitrate,
                                                 const OggComments &comments) {

    if (samples.empty()) {
        return Result<std::vector<uint8_t>>{
            ServerError(ServerError::InvalidData, "Cannot encode an empty sound to Ogg/Opus")};
    }
    if (sampleRate != kShareableSampleRate) {
        return Result<std::vector<uint8_t>>{
            ServerError(ServerError::InvalidData,
                        fmt::format("Expected {} Hz audio (the pipeline is 48kHz by design) but this file is {} Hz",
                                    kShareableSampleRate, sampleRate))};
    }

    int opusError = OPUS_OK;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
        opus_encoder_create(kShareableSampleRate, 1, OPUS_APPLICATION_AUDIO, &opusError), opus_encoder_destroy);
    if (opusError != OPUS_OK || !encoder) {
        return Result<std::vector<uint8_t>>{ServerError(
            ServerError::InternalError, fmt::format("opus_encoder_create failed: {}", opus_strerror(opusError)))};
    }
    opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(10));

    // The encoder's algorithmic delay is exactly what OpusHead's pre-skip tells the
    // decoder to trim, so the shared audio starts at the true first sample.
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead));

    OggStreamGuard ogg;
    if (ogg_stream_init(&ogg.state, kStreamSerial) != 0) {
        return Result<std::vector<uint8_t>>{ServerError(ServerError::InternalError, "ogg_stream_init failed")};
    }
    ogg.initialized = true;

    std::vector<uint8_t> out;
    ogg_page page;

    auto head = buildOpusHead(static_cast<uint16_t>(lookahead), kShareableSampleRate);
    ogg_packet headPacket{};
    headPacket.packet = head.data();
    headPacket.bytes = static_cast<long>(head.size());
    headPacket.b_o_s = 1;
    headPacket.packetno = 0;
    ogg_stream_packetin(&ogg.state, &headPacket);
    while (ogg_stream_flush(&ogg.state, &page)) {
        appendPage(out, page);
    }

    auto tags = buildOpusTags(comments);
    ogg_packet tagsPacket{};
    tagsPacket.packet = tags.data();
    tagsPacket.bytes = static_cast<long>(tags.size());
    tagsPacket.packetno = 1;
    ogg_stream_packetin(&ogg.state, &tagsPacket);
    while (ogg_stream_flush(&ogg.state, &page)) {
        appendPage(out, page);
    }

    std::vector<int16_t> frame(kFrameSamples);
    std::vector<uint8_t> packetBuffer(4096);
    const auto totalSamples = static_cast<ogg_int64_t>(samples.size());
    ogg_int64_t encoded = 0;
    ogg_int64_t rawDecodedSamples = 0;
    ogg_int64_t packetNumber = 2;

    while (encoded < totalSamples) {
        const auto take = std::min<ogg_int64_t>(kFrameSamples, totalSamples - encoded);
        std::copy_n(samples.begin() + encoded, take, frame.begin());
        std::fill(frame.begin() + take, frame.end(), int16_t{0}); // pad the final frame with silence

        const auto packetBytes = opus_encode(encoder.get(), frame.data(), kFrameSamples, packetBuffer.data(),
                                             static_cast<opus_int32>(packetBuffer.size()));
        if (packetBytes < 0) {
            return Result<std::vector<uint8_t>>{ServerError(
                ServerError::InternalError, fmt::format("opus_encode failed: {}", opus_strerror(packetBytes)))};
        }

        encoded += take;
        rawDecodedSamples += kFrameSamples;
        const bool lastPacket = encoded >= totalSamples;

        ogg_packet packet{};
        packet.packet = packetBuffer.data();
        packet.bytes = packetBytes;
        // Granule positions count RAW decoded samples at 48kHz — the encoder's delay is
        // already inside them, so nothing is added for pre-skip. The FINAL position is
        // pulled back to pre-skip + real length, which is how decoders know to trim both
        // the leading encoder delay and the trailing silence padding (RFC 7845 §4).
        packet.granulepos = lastPacket ? (lookahead + totalSamples) : rawDecodedSamples;
        packet.packetno = packetNumber++;
        packet.e_o_s = lastPacket ? 1 : 0;
        ogg_stream_packetin(&ogg.state, &packet);

        while (ogg_stream_pageout(&ogg.state, &page)) {
            appendPage(out, page);
        }
    }

    while (ogg_stream_flush(&ogg.state, &page)) {
        appendPage(out, page);
    }

    debug("encoded {} samples to Ogg/Opus: {} bytes at {} bps", samples.size(), out.size(), bitrate);
    return Result<std::vector<uint8_t>>{out};
}

} // namespace creatures::audio
