#include "WavFileReader.h"

#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <vector>

#include "server/namespace-stuffs.h"

namespace creatures::voice {

namespace {

template <typename T> bool readLE(std::ifstream &in, T &out) {
    in.read(reinterpret_cast<char *>(&out), sizeof(T));
    return in.good();
}

bool readBytes(std::ifstream &in, char *buf, std::size_t n) {
    in.read(buf, static_cast<std::streamsize>(n));
    return in.good();
}

} // namespace

Result<WavInfo> readWavInfo(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Result<WavInfo>{
            ServerError(ServerError::InvalidData, fmt::format("Cannot open WAV file: {}", path.string()))};
    }

    char riff[4]{};
    uint32_t riffSize = 0;
    char wave[4]{};
    if (!readBytes(in, riff, 4) || !readLE(in, riffSize) || !readBytes(in, wave, 4)) {
        return Result<WavInfo>{
            ServerError(ServerError::InvalidData, fmt::format("WAV header truncated: {}", path.string()))};
    }
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return Result<WavInfo>{
            ServerError(ServerError::InvalidData, fmt::format("Not a RIFF/WAVE file: {}", path.string()))};
    }

    WavInfo info{};
    bool sawFmt = false;
    bool sawData = false;

    // Walk subchunks until we've got fmt + data. WAVs in the wild can have
    // extra chunks (LIST, JUNK, bext, etc.) between fmt and data — skip them.
    while (in.good() && !(sawFmt && sawData)) {
        char chunkId[4]{};
        uint32_t chunkSize = 0;
        if (!readBytes(in, chunkId, 4) || !readLE(in, chunkSize)) {
            break;
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                return Result<WavInfo>{
                    ServerError(ServerError::InvalidData,
                                fmt::format("WAV fmt chunk too small ({}) in {}", chunkSize, path.string()))};
            }
            uint16_t audioFormat = 0;
            uint16_t numChannels = 0;
            uint32_t sampleRate = 0;
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            uint16_t bitsPerSample = 0;
            if (!readLE(in, audioFormat) || !readLE(in, numChannels) || !readLE(in, sampleRate) ||
                !readLE(in, byteRate) || !readLE(in, blockAlign) || !readLE(in, bitsPerSample)) {
                return Result<WavInfo>{
                    ServerError(ServerError::InvalidData, fmt::format("WAV fmt chunk truncated in {}", path.string()))};
            }
            info.audioFormat = audioFormat;
            info.numChannels = numChannels;
            info.sampleRate = sampleRate;
            info.bitsPerSample = bitsPerSample;
            sawFmt = true;

            // Skip any fmt extension bytes past the 16 we consumed.
            if (chunkSize > 16) {
                in.seekg(chunkSize - 16, std::ios::cur);
            }
            // Chunks are padded to even sizes; skip the trailing byte if any.
            if (chunkSize % 2 == 1) {
                in.seekg(1, std::ios::cur);
            }
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            info.dataOffset = static_cast<std::uint64_t>(in.tellg());
            info.dataBytes = chunkSize;
            sawData = true;
            // Stop — we know fmt came first (it always does in canonical WAV).
            break;
        } else {
            // Unknown chunk; skip its payload (+1 if odd-sized for padding).
            std::streamoff skip = static_cast<std::streamoff>(chunkSize);
            if (chunkSize % 2 == 1)
                skip += 1;
            in.seekg(skip, std::ios::cur);
        }
    }

    if (!sawFmt) {
        return Result<WavInfo>{
            ServerError(ServerError::InvalidData, fmt::format("WAV missing fmt chunk: {}", path.string()))};
    }
    if (!sawData) {
        return Result<WavInfo>{
            ServerError(ServerError::InvalidData, fmt::format("WAV missing data chunk: {}", path.string()))};
    }
    if (info.audioFormat != 1) {
        return Result<WavInfo>{
            ServerError(ServerError::InvalidData,
                        fmt::format("WAV is not PCM (audioFormat={}) in {}", info.audioFormat, path.string()))};
    }
    return Result<WavInfo>{info};
}

Result<int> readWavChannelCount(const std::filesystem::path &path) {
    auto infoResult = readWavInfo(path);
    if (!infoResult.isSuccess()) {
        return Result<int>{infoResult.getError().value()};
    }
    return Result<int>{static_cast<int>(infoResult.getValue().value().numChannels)};
}

Result<void> extractChannelToMonoWav(const std::filesystem::path &sourcePath, const std::filesystem::path &outputPath,
                                     int channelIndex) {
    auto infoResult = readWavInfo(sourcePath);
    if (!infoResult.isSuccess()) {
        return Result<void>{infoResult.getError().value()};
    }
    const auto info = infoResult.getValue().value();

    if (info.bitsPerSample != 16) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                        fmt::format("WAV {} is {}-bit; extractChannelToMonoWav only supports 16-bit",
                                                    sourcePath.string(), info.bitsPerSample))};
    }
    if (channelIndex < 1 || channelIndex > static_cast<int>(info.numChannels)) {
        return Result<void>{
            ServerError(ServerError::InvalidData, fmt::format("channelIndex {} out of range [1, {}] for {}",
                                                              channelIndex, info.numChannels, sourcePath.string()))};
    }

    std::ifstream in(sourcePath, std::ios::binary);
    if (!in.is_open()) {
        return Result<void>{
            ServerError(ServerError::InvalidData, fmt::format("Cannot reopen WAV: {}", sourcePath.string()))};
    }
    in.seekg(static_cast<std::streamoff>(info.dataOffset), std::ios::beg);

    constexpr std::size_t bytesPerSample = 2; // 16-bit
    const std::size_t blockBytes = static_cast<std::size_t>(info.numChannels) * bytesPerSample;
    if (blockBytes == 0) {
        return Result<void>{ServerError(ServerError::InvalidData, "WAV has zero-byte sample blocks")};
    }
    const std::size_t totalFrames = info.dataBytes / blockBytes;
    const std::size_t targetByteOffset = static_cast<std::size_t>(channelIndex - 1) * bytesPerSample;

    // Read the whole data section into memory. For a 7-second 17-channel 48 kHz
    // S16 WAV this is ~11 MB — easily fits, simpler than streaming.
    std::vector<uint8_t> raw(info.dataBytes);
    in.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(info.dataBytes));
    if (in.gcount() != static_cast<std::streamsize>(info.dataBytes)) {
        return Result<void>{
            ServerError(ServerError::InvalidData,
                        fmt::format("WAV data chunk shorter than advertised in {}", sourcePath.string()))};
    }

    std::vector<int16_t> mono(totalFrames);
    for (std::size_t f = 0; f < totalFrames; ++f) {
        const std::size_t srcOffset = f * blockBytes + targetByteOffset;
        int16_t sample = 0;
        std::memcpy(&sample, &raw[srcOffset], bytesPerSample);
        mono[f] = sample;
    }

    // Write a canonical 44-byte PCM mono WAV.
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open()) {
        return Result<void>{
            ServerError(ServerError::InvalidData, fmt::format("Cannot create mono WAV: {}", outputPath.string()))};
    }
    const uint32_t dataBytes = static_cast<uint32_t>(mono.size() * bytesPerSample);
    const uint32_t fileSize = 36 + dataBytes;
    const uint16_t monoChannels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t fmtSize = 16;
    const uint16_t audioFormat = 1;
    const uint32_t byteRate = info.sampleRate * monoChannels * bytesPerSample;
    const uint16_t blockAlign = monoChannels * bytesPerSample;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char *>(&fileSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char *>(&fmtSize), 4);
    out.write(reinterpret_cast<const char *>(&audioFormat), 2);
    out.write(reinterpret_cast<const char *>(&monoChannels), 2);
    out.write(reinterpret_cast<const char *>(&info.sampleRate), 4);
    out.write(reinterpret_cast<const char *>(&byteRate), 4);
    out.write(reinterpret_cast<const char *>(&blockAlign), 2);
    out.write(reinterpret_cast<const char *>(&bitsPerSample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char *>(&dataBytes), 4);
    out.write(reinterpret_cast<const char *>(mono.data()), dataBytes);
    if (!out.good()) {
        return Result<void>{
            ServerError(ServerError::InvalidData, fmt::format("Failed writing mono WAV: {}", outputPath.string()))};
    }
    debug("Extracted channel {} of {} from {} → {} ({} samples)", channelIndex, info.numChannels, sourcePath.string(),
          outputPath.string(), totalFrames);
    return Result<void>{};
}

} // namespace creatures::voice
