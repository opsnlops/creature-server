#include "PcmWavWriter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

#include <fmt/format.h>

#include "server/config.h"
#include "server/namespace-stuffs.h"

namespace creatures::voice {

Result<std::size_t> writePcmToMultichannelWav(const std::vector<uint8_t> &pcmData, const std::filesystem::path &wavPath,
                                              uint16_t audioChannel, uint32_t sampleRate) {
    const uint16_t totalChannels = RTP_STREAMING_CHANNELS;
    const uint16_t bitsPerSample = 16;
    const uint16_t bytesPerSample = bitsPerSample / 8;

    const std::size_t monoSamples = pcmData.size() / bytesPerSample;
    const std::size_t dataSize = monoSamples * totalChannels * bytesPerSample;

    std::ofstream file(wavPath, std::ios::binary);
    if (!file.is_open()) {
        return Result<std::size_t>{
            ServerError(ServerError::InternalError, fmt::format("Cannot create WAV file: {}", wavPath.string()))};
    }

    // RIFF header
    const uint32_t fileSize = static_cast<uint32_t>(36 + dataSize);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char *>(&fileSize), 4);
    file.write("WAVE", 4);

    // fmt chunk
    file.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    file.write(reinterpret_cast<const char *>(&fmtSize), 4);
    const uint16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<const char *>(&audioFormat), 2);
    file.write(reinterpret_cast<const char *>(&totalChannels), 2);
    file.write(reinterpret_cast<const char *>(&sampleRate), 4);
    const uint32_t byteRate = sampleRate * totalChannels * bytesPerSample;
    file.write(reinterpret_cast<const char *>(&byteRate), 4);
    const uint16_t blockAlign = totalChannels * bytesPerSample;
    file.write(reinterpret_cast<const char *>(&blockAlign), 2);
    file.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

    // data chunk
    file.write("data", 4);
    const auto dataSizeU32 = static_cast<uint32_t>(dataSize);
    file.write(reinterpret_cast<const char *>(&dataSizeU32), 4);

    // Interleaved samples: source on `audioChannel`, silence elsewhere.
    const uint16_t targetIdx = static_cast<uint16_t>(audioChannel - 1);
    const int16_t silence = 0;
    const auto *monoPtr = reinterpret_cast<const int16_t *>(pcmData.data());
    for (std::size_t i = 0; i < monoSamples; ++i) {
        for (uint16_t ch = 0; ch < totalChannels; ++ch) {
            if (ch == targetIdx) {
                file.write(reinterpret_cast<const char *>(&monoPtr[i]), 2);
            } else {
                file.write(reinterpret_cast<const char *>(&silence), 2);
            }
        }
    }
    file.close();

    const std::size_t totalSize = 44 + dataSize;
    debug("Wrote 17-channel WAV: {} samples on channel {}, {} bytes total", monoSamples, audioChannel, totalSize);
    return totalSize;
}

namespace {

// Where one WAV's audio lives, plus the format needed to validate siblings.
struct WavDataChunk {
    uint16_t channels{0};
    uint32_t sampleRate{0};
    uint16_t bitsPerSample{0};
    std::streamoff dataOffset{0};
    uint64_t dataSize{0};
};

// Walk a RIFF/WAVE file's chunks and pull out the fmt fields + data location.
Result<WavDataChunk> locateWavData(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return Result<WavDataChunk>{
            ServerError(ServerError::InvalidData, fmt::format("Cannot open WAV part: {}", path.string()))};
    }
    auto readU16 = [&](uint16_t &v) { file.read(reinterpret_cast<char *>(&v), 2); };
    auto readU32 = [&](uint32_t &v) { file.read(reinterpret_cast<char *>(&v), 4); };

    char tag[4];
    uint32_t riffSize = 0;
    file.read(tag, 4);
    readU32(riffSize);
    char wave[4];
    file.read(wave, 4);
    if (!file.good() || std::memcmp(tag, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return Result<WavDataChunk>{
            ServerError(ServerError::InvalidData, fmt::format("Not a RIFF/WAVE file: {}", path.string()))};
    }

    WavDataChunk info;
    bool haveFmt = false;
    while (file.good()) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        file.read(chunkId, 4);
        readU32(chunkSize);
        if (!file.good()) {
            break;
        }
        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0;
            readU16(audioFormat);
            readU16(info.channels);
            readU32(info.sampleRate);
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            readU32(byteRate);
            readU16(blockAlign);
            readU16(info.bitsPerSample);
            if (audioFormat != 1) {
                return Result<WavDataChunk>{ServerError(ServerError::InvalidData,
                                                        fmt::format("WAV part is not integer PCM: {}", path.string()))};
            }
            haveFmt = true;
            // Skip whatever's left of an extended fmt chunk.
            file.seekg(static_cast<std::streamoff>(chunkSize) - 16, std::ios::cur);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            info.dataOffset = file.tellg();
            info.dataSize = chunkSize;
            file.seekg(static_cast<std::streamoff>(chunkSize + (chunkSize % 2)), std::ios::cur);
        } else {
            file.seekg(static_cast<std::streamoff>(chunkSize + (chunkSize % 2)), std::ios::cur);
        }
    }
    if (!haveFmt || info.dataSize == 0) {
        return Result<WavDataChunk>{
            ServerError(ServerError::InvalidData, fmt::format("WAV part has no fmt/data chunk: {}", path.string()))};
    }
    return info;
}

} // namespace

Result<StitchedWavInfo> stitchMultichannelWavs(const std::vector<std::filesystem::path> &parts,
                                               const std::filesystem::path &outPath, const WavProvenance &provenance) {
    if (parts.empty()) {
        return Result<StitchedWavInfo>{ServerError(ServerError::InvalidData, "No WAV parts to stitch")};
    }

    // First pass: locate every part's audio and check the formats agree.
    std::vector<WavDataChunk> chunks;
    chunks.reserve(parts.size());
    uint64_t totalDataSize = 0;
    for (const auto &part : parts) {
        auto located = locateWavData(part);
        if (!located.isSuccess()) {
            return Result<StitchedWavInfo>{located.getError().value()};
        }
        const auto &chunk = located.getValue().value();
        const auto &first = chunks.empty() ? chunk : chunks.front();
        if (chunk.channels != first.channels || chunk.sampleRate != first.sampleRate ||
            chunk.bitsPerSample != first.bitsPerSample) {
            return Result<StitchedWavInfo>{
                ServerError(ServerError::InvalidData, fmt::format("WAV part format mismatch: {}", part.string()))};
        }
        totalDataSize += chunk.dataSize;
        chunks.push_back(chunk);
    }
    const auto &format = chunks.front();

    // Optional iXML chunk, folded into the RIFF size like wrapMonoPcmAsWav does.
    std::vector<uint8_t> ixmlChunk;
    if (!provenance.empty()) {
        ixmlChunk = makeIxmlChunk(buildIxml(provenance, format.channels));
    }

    const uint64_t riffSize = 36 + totalDataSize + ixmlChunk.size();
    if (riffSize > std::numeric_limits<uint32_t>::max()) {
        return Result<StitchedWavInfo>{
            ServerError(ServerError::InvalidData, "Stitched WAV would exceed the RIFF 4 GB limit")};
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out.is_open()) {
        return Result<StitchedWavInfo>{
            ServerError(ServerError::InternalError, fmt::format("Cannot create WAV file: {}", outPath.string()))};
    }
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    out.write("RIFF", 4);
    u32(static_cast<uint32_t>(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1); // PCM
    u16(format.channels);
    u32(format.sampleRate);
    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    u32(format.sampleRate * format.channels * bytesPerSample);
    u16(static_cast<uint16_t>(format.channels * bytesPerSample));
    u16(format.bitsPerSample);
    out.write("data", 4);
    u32(static_cast<uint32_t>(totalDataSize));

    // Second pass: stream each part's data chunk into place.
    StitchedWavInfo stitched;
    const uint64_t bytesPerMs = static_cast<uint64_t>(format.sampleRate) * format.channels * bytesPerSample / 1000ull;
    std::vector<char> buffer(1 << 20);
    for (size_t i = 0; i < parts.size(); i++) {
        std::ifstream in(parts[i], std::ios::binary);
        in.seekg(chunks[i].dataOffset);
        uint64_t remaining = chunks[i].dataSize;
        while (remaining > 0 && in.good()) {
            const auto want = static_cast<std::streamsize>(std::min<uint64_t>(remaining, buffer.size()));
            in.read(buffer.data(), want);
            const auto got = in.gcount();
            if (got <= 0) {
                break;
            }
            out.write(buffer.data(), got);
            remaining -= static_cast<uint64_t>(got);
        }
        if (remaining > 0) {
            return Result<StitchedWavInfo>{ServerError(
                ServerError::InternalError, fmt::format("Short read stitching WAV part: {}", parts[i].string()))};
        }
        stitched.partDurationsMs.push_back(bytesPerMs == 0 ? 0 : chunks[i].dataSize / bytesPerMs);
    }
    out.write(reinterpret_cast<const char *>(ixmlChunk.data()), static_cast<std::streamsize>(ixmlChunk.size()));
    out.close();
    if (!out.good()) {
        return Result<StitchedWavInfo>{
            ServerError(ServerError::InternalError, fmt::format("Failed writing stitched WAV: {}", outPath.string()))};
    }

    stitched.totalDurationMs = bytesPerMs == 0 ? 0 : totalDataSize / bytesPerMs;
    debug("Stitched {} WAV parts into {} ({} ms, {} bytes of PCM)", parts.size(), outPath.string(),
          stitched.totalDurationMs, totalDataSize);
    return stitched;
}

std::vector<uint8_t> wrapMonoPcmAsWav(const std::vector<uint8_t> &pcm, uint32_t sampleRate,
                                      const WavProvenance *provenance) {
    // Optional iXML provenance chunk (#50), appended after `data`. Built up front
    // so its size folds into the RIFF container size.
    std::vector<uint8_t> ixmlChunk;
    if (provenance && !provenance->empty()) {
        ixmlChunk = makeIxmlChunk(buildIxml(*provenance));
    }

    std::vector<uint8_t> out;
    out.reserve(44 + pcm.size() + ixmlChunk.size());
    auto u16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto str = [&](const char *s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(static_cast<uint8_t>(s[i]));
        }
    };
    const uint32_t dataLen = static_cast<uint32_t>(pcm.size());
    str("RIFF", 4);
    u32(static_cast<uint32_t>(36 + dataLen + ixmlChunk.size()));
    str("WAVE", 4);
    str("fmt ", 4);
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(sampleRate);
    u32(sampleRate * 2); // byte rate (mono S16)
    u16(2);              // block align
    u16(16);             // bits per sample
    str("data", 4);
    u32(dataLen);
    out.insert(out.end(), pcm.begin(), pcm.end());
    out.insert(out.end(), ixmlChunk.begin(), ixmlChunk.end());
    return out;
}

} // namespace creatures::voice
