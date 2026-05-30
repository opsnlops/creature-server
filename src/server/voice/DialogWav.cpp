#include "DialogWav.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "server/config.h"
#include "server/namespace-stuffs.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

namespace {

/// Compile-time guarantee that the streaming-audio contract matches what we
/// hard-code as our output shape. If either constant ever changes this file
/// breaks fast at build time rather than silently writing a wrong-shape WAV.
static_assert(RTP_STREAMING_CHANNELS == 17, "DialogWav writes a 17-channel WAV");
static_assert(RTP_SRATE == 48000, "DialogWav writes a 48 kHz WAV");

constexpr uint16_t kBitsPerSample = 16;
constexpr uint16_t kBytesPerSample = kBitsPerSample / 8;
constexpr uint16_t kWavFormatPCM = 1;
constexpr std::size_t kWavHeaderBytes = 44;

/// Little-endian writers. WAV is always LE; macOS/Linux on x86_64 + arm64 are
/// LE too, so a plain write would work — but explicit byte writes keep this
/// correct on any host and document the on-disk format.
void writeU16LE(std::ostream &os, uint16_t v) {
    const std::array<char, 2> b{static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    os.write(b.data(), b.size());
}
void writeU32LE(std::ostream &os, uint32_t v) {
    const std::array<char, 4> b{static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                                static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    os.write(b.data(), b.size());
}

} // namespace

Result<void> writeDialogWav(const DialogAssembled &assembled, const VoiceChannelMap &voiceToChannel,
                            const std::filesystem::path &outPath, std::shared_ptr<OperationSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("DialogWav.writeDialogWav", parentSpan);
    if (span) {
        span->setAttribute("wav.path", outPath.string());
        span->setAttribute("wav.voices", static_cast<int64_t>(assembled.perCreature.size()));
        span->setAttribute("wav.total_samples", static_cast<int64_t>(assembled.totalSamples));
        span->setAttribute("wav.sample_rate", static_cast<int64_t>(assembled.sampleRate));
    }

    // ---- Input validation.
    if (assembled.sampleRate != static_cast<uint32_t>(RTP_SRATE)) {
        std::string msg =
            fmt::format("writeDialogWav: sample rate {} is not the required {} Hz", assembled.sampleRate, RTP_SRATE);
        error(msg);
        if (span)
            span->setError(msg);
        return Result<void>{ServerError(ServerError::InvalidData, msg)};
    }
    if (assembled.totalSamples == 0) {
        std::string msg = "writeDialogWav: assembled scene has 0 samples";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<void>{ServerError(ServerError::InvalidData, msg)};
    }
    if (assembled.perCreature.empty()) {
        std::string msg = "writeDialogWav: assembled scene has no voices";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<void>{ServerError(ServerError::InvalidData, msg)};
    }

    // Resolve each voice → lane (0-based). Check lane is in [0, 17), and that
    // no two voices map to the same lane. A collision would silently clobber
    // one creature's audio, so it's a hard error.
    struct VoiceLane {
        std::size_t perCreatureIndex;
        uint8_t lane;
    };
    std::vector<VoiceLane> lanes;
    lanes.reserve(assembled.perCreature.size());
    std::unordered_set<uint16_t> usedChannels;
    for (std::size_t i = 0; i < assembled.perCreature.size(); ++i) {
        const auto &pc = assembled.perCreature[i];
        auto it = voiceToChannel.find(pc.voiceId);
        if (it == voiceToChannel.end()) {
            std::string msg =
                fmt::format("writeDialogWav: voice '{}' is in the scene but not in the channel map", pc.voiceId);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<void>{ServerError(ServerError::InvalidData, msg)};
        }
        const uint16_t ch = it->second;
        if (ch < 1 || ch > RTP_STREAMING_CHANNELS) {
            std::string msg = fmt::format("writeDialogWav: voice '{}' has audio_channel {} (must be 1..{})", pc.voiceId,
                                          ch, RTP_STREAMING_CHANNELS);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<void>{ServerError(ServerError::InvalidData, msg)};
        }
        if (!usedChannels.insert(ch).second) {
            std::string msg =
                fmt::format("writeDialogWav: audio_channel {} is assigned to more than one voice in this scene", ch);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<void>{ServerError(ServerError::InvalidData, msg)};
        }
        if (pc.pcm.size() != assembled.totalSamples) {
            std::string msg = fmt::format("writeDialogWav: voice '{}' PCM length {} != assembled totalSamples {}",
                                          pc.voiceId, pc.pcm.size(), assembled.totalSamples);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<void>{ServerError(ServerError::InvalidData, msg)};
        }
        lanes.push_back({i, static_cast<uint8_t>(ch - 1)});
    }

    // WAV size-field cap: dataLen and the RIFF chunk size are 32-bit, so the
    // file (header + data) must fit in 2^32 bytes. Realistic dialog scenes are
    // well under this — a 5-minute 17-channel 48k S16 file is ~490 MiB — but
    // reject explicitly rather than silently truncate the size field.
    const std::uint64_t totalChannels = RTP_STREAMING_CHANNELS;
    const std::uint64_t dataBytes64 =
        static_cast<std::uint64_t>(assembled.totalSamples) * totalChannels * kBytesPerSample;
    if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() - kWavHeaderBytes) {
        std::string msg = fmt::format(
            "writeDialogWav: output {} bytes exceeds the 4 GiB WAV size-field cap (scene too long)", dataBytes64);
        error(msg);
        if (span)
            span->setError(msg);
        return Result<void>{ServerError(ServerError::InvalidData, msg)};
    }
    const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);

    // ---- Interleave: build the full output buffer in memory.
    //
    // Walk the timeline once. For each sample t, write 17 lanes back-to-back;
    // each lane that this scene assigned to a creature gets that creature's
    // sample at t; all other lanes stay zero.
    std::vector<int16_t> interleaved(static_cast<std::size_t>(assembled.totalSamples) * RTP_STREAMING_CHANNELS, 0);
    for (const auto &vl : lanes) {
        const auto &src = assembled.perCreature[vl.perCreatureIndex].pcm;
        // Scatter src[t] → interleaved[t * 17 + lane].
        for (std::size_t t = 0; t < assembled.totalSamples; ++t) {
            interleaved[t * RTP_STREAMING_CHANNELS + vl.lane] = src[t];
        }
    }

    // ---- Write the file.
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::string msg = fmt::format("writeDialogWav: failed to open {} for writing", outPath.string());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    // Canonical 44-byte PCM WAV header.
    out.write("RIFF", 4);
    writeU32LE(out, static_cast<std::uint32_t>(36 + dataBytes));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32LE(out, 16); // PCM fmt chunk size
    writeU16LE(out, kWavFormatPCM);
    writeU16LE(out, static_cast<std::uint16_t>(RTP_STREAMING_CHANNELS));
    writeU32LE(out, static_cast<std::uint32_t>(RTP_SRATE));
    writeU32LE(out, static_cast<std::uint32_t>(RTP_SRATE) * RTP_STREAMING_CHANNELS * kBytesPerSample); // byte rate
    writeU16LE(out, static_cast<std::uint16_t>(RTP_STREAMING_CHANNELS * kBytesPerSample));             // block align
    writeU16LE(out, kBitsPerSample);
    out.write("data", 4);
    writeU32LE(out, dataBytes);

    // Sample data — int16_t is host-endian, host is LE on our targets.
    static_assert(sizeof(int16_t) == 2, "int16_t is exactly 2 bytes");
    out.write(reinterpret_cast<const char *>(interleaved.data()),
              static_cast<std::streamsize>(interleaved.size() * sizeof(int16_t)));

    out.flush();
    if (!out) {
        std::string msg = fmt::format("writeDialogWav: write or flush failed for {}", outPath.string());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    const double durationS = static_cast<double>(assembled.totalSamples) / static_cast<double>(RTP_SRATE);
    info("writeDialogWav: wrote {} ({} samples, {} voices on lanes, {:.2f}s, {} bytes data)", outPath.string(),
         assembled.totalSamples, lanes.size(), durationS, dataBytes);

    if (span) {
        span->setAttribute("wav.bytes", static_cast<int64_t>(kWavHeaderBytes + dataBytes));
        span->setAttribute("wav.duration_s", durationS);
        span->setAttribute("wav.lanes_used", static_cast<int64_t>(lanes.size()));
        span->setSuccess();
    }
    return Result<void>{};
}

} // namespace creatures::voice
