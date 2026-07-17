
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <lame/lame.h>
#include <spdlog/spdlog.h>

#include "server/audio/Mp3Writer.h"
#include "server/namespace-stuffs.h"

namespace creatures::audio {

namespace {

// Encode this many samples per lame_encode_buffer() call. Keeps the per-call mp3
// scratch buffer small and bounded instead of sizing one buffer to the whole clip.
constexpr int kBlockSamples = 8192;

// LAME's documented worst-case output size for a block of n input samples is
// 1.25*n + 7200 bytes; the flush call needs up to 7200 on its own.
constexpr int kFlushReserve = 7200;

// ID3v2 sizes are "syncsafe": 28 bits spread across 4 bytes, 7 bits each, high bit
// of every byte zero (so the tag can never contain a false MPEG frame sync).
void appendSyncsafe32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 21) & 0x7f));
    out.push_back(static_cast<uint8_t>((value >> 14) & 0x7f));
    out.push_back(static_cast<uint8_t>((value >> 7) & 0x7f));
    out.push_back(static_cast<uint8_t>(value & 0x7f));
}

// A minimal ID3v2.4.0 tag carrying one TXXX (user-defined text) frame per comment, so a
// shared .mp3 mirrors the dialog provenance the Ogg endpoint puts in OpusTags (#47, #57).
// Each TXXX body is: encoding byte 0x03 (UTF-8), the key as the frame's "description",
// a 0x00 terminator, then the value. v2.4 frame sizes are syncsafe.
std::vector<uint8_t> buildId3v2Tag(const Id3Comments &comments) {
    std::vector<uint8_t> frames;
    for (const auto &[key, value] : comments) {
        std::vector<uint8_t> body;
        body.push_back(0x03); // UTF-8
        body.insert(body.end(), key.begin(), key.end());
        body.push_back(0x00); // description/value separator
        body.insert(body.end(), value.begin(), value.end());

        const std::string frameId = "TXXX";
        frames.insert(frames.end(), frameId.begin(), frameId.end());
        appendSyncsafe32(frames, static_cast<uint32_t>(body.size()));
        frames.push_back(0x00); // frame flags
        frames.push_back(0x00);
        frames.insert(frames.end(), body.begin(), body.end());
    }

    std::vector<uint8_t> tag;
    const std::string magic = "ID3";
    tag.insert(tag.end(), magic.begin(), magic.end());
    tag.push_back(0x04);                                         // version 2.4.0
    tag.push_back(0x00);                                         // revision
    tag.push_back(0x00);                                         // flags
    appendSyncsafe32(tag, static_cast<uint32_t>(frames.size())); // size excludes the 10-byte header
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

struct LameGuard {
    lame_global_flags *gfp = nullptr;
    ~LameGuard() {
        if (gfp) {
            lame_close(gfp);
        }
    }
};

} // namespace

Result<std::vector<uint8_t>> encodeMonoToMp3(const std::vector<int16_t> &samples, int sampleRate, int bitrate,
                                             const Id3Comments &comments) {

    if (samples.empty()) {
        return Result<std::vector<uint8_t>>{
            ServerError(ServerError::InvalidData, "Cannot encode an empty sound to MP3")};
    }
    if (sampleRate != kShareableSampleRate) {
        return Result<std::vector<uint8_t>>{
            ServerError(ServerError::InvalidData,
                        fmt::format("Expected {} Hz audio (the pipeline is 48kHz by design) but this file is {} Hz",
                                    kShareableSampleRate, sampleRate))};
    }

    LameGuard lame;
    lame.gfp = lame_init();
    if (!lame.gfp) {
        return Result<std::vector<uint8_t>>{ServerError(ServerError::InternalError, "lame_init failed")};
    }

    lame_set_in_samplerate(lame.gfp, kShareableSampleRate);
    lame_set_out_samplerate(lame.gfp, kShareableSampleRate); // no resampling — keep 48 kHz
    lame_set_num_channels(lame.gfp, 1);
    lame_set_mode(lame.gfp, MONO);
    lame_set_VBR(lame.gfp, vbr_off);          // constant bitrate
    lame_set_brate(lame.gfp, bitrate / 1000); // LAME wants kbps
    lame_set_quality(lame.gfp, 2);            // 0=best/slowest .. 9=worst; 2 is the high-quality sweet spot
    // No Xing/Info (VBR) header: writing it in-memory means seeking back to overwrite the
    // reserved first frame, and CBR players compute duration fine without it. Also makes the
    // output deterministic (mirrors the byte-stable Ogg encoder). See docs/mp3-rendition-plan.md.
    lame_set_bWriteVbrTag(lame.gfp, 0);

    if (lame_init_params(lame.gfp) < 0) {
        return Result<std::vector<uint8_t>>{ServerError(ServerError::InternalError, "lame_init_params failed")};
    }

    std::vector<uint8_t> out;
    if (!comments.empty()) {
        const auto tag = buildId3v2Tag(comments);
        out.insert(out.end(), tag.begin(), tag.end());
    }

    std::vector<uint8_t> mp3buf;
    const auto total = samples.size();
    size_t pos = 0;
    while (pos < total) {
        const int n = static_cast<int>(std::min<size_t>(kBlockSamples, total - pos));
        mp3buf.resize(static_cast<size_t>(n) + static_cast<size_t>(n) / 4 + kFlushReserve);
        // Mono input: LAME reads only buffer_l when num_channels==1; pass the same
        // pointer for buffer_r so no version-specific NULL handling matters.
        const int bytes = lame_encode_buffer(lame.gfp, samples.data() + pos, samples.data() + pos, n, mp3buf.data(),
                                             static_cast<int>(mp3buf.size()));
        if (bytes < 0) {
            return Result<std::vector<uint8_t>>{
                ServerError(ServerError::InternalError, fmt::format("lame_encode_buffer failed: {}", bytes))};
        }
        out.insert(out.end(), mp3buf.begin(), mp3buf.begin() + bytes);
        pos += static_cast<size_t>(n);
    }

    mp3buf.resize(kFlushReserve);
    const int flushed = lame_encode_flush(lame.gfp, mp3buf.data(), static_cast<int>(mp3buf.size()));
    if (flushed < 0) {
        return Result<std::vector<uint8_t>>{
            ServerError(ServerError::InternalError, fmt::format("lame_encode_flush failed: {}", flushed))};
    }
    out.insert(out.end(), mp3buf.begin(), mp3buf.begin() + flushed);

    debug("encoded {} samples to MP3: {} bytes at {} bps", samples.size(), out.size(), bitrate);
    return Result<std::vector<uint8_t>>{out};
}

} // namespace creatures::audio
