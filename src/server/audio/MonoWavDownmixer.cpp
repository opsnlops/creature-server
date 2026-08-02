//
// MonoWavDownmixer.cpp
// Renders multi-channel WAV files down to mono for travel mode
//

#include "MonoWavDownmixer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>

#include <fmt/format.h>

#include "server/namespace-stuffs.h"

namespace creatures::audio {

namespace {

constexpr uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr size_t MAX_FMT_CHUNK_BYTES = 64;
constexpr uint16_t MAX_WAV_CHANNELS = 64;
constexpr int MIN_WAV_SAMPLE_RATE = 8000;
constexpr int MAX_WAV_SAMPLE_RATE = 384000;
constexpr size_t MAX_STREAM_READ_FRAMES = 4096;

uint16_t readLittleEndianU16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLittleEndianU32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool readExact(std::ifstream &file, void *destination, size_t byteCount) {
    file.read(static_cast<char *>(destination), static_cast<std::streamsize>(byteCount));
    return file.good() || static_cast<size_t>(file.gcount()) == byteCount;
}

bool isPcmExtensibleSubformat(const std::vector<uint8_t> &formatData) {
    // KSDATAFORMAT_SUBTYPE_PCM = 00000001-0000-0010-8000-00aa00389b71.
    static constexpr std::array<uint8_t, 16> PCM_SUBFORMAT = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    };

    return formatData.size() >= 40 && std::equal(PCM_SUBFORMAT.begin(), PCM_SUBFORMAT.end(), formatData.begin() + 24);
}

} // namespace

void downmixToMono(const int16_t *interleaved, const size_t frames, const int channels, int16_t *out) {
    for (size_t f = 0; f < frames; ++f) {
        const int16_t *frameBase = interleaved + f * static_cast<size_t>(channels);
        int32_t acc = 0;
        for (int ch = 0; ch < channels; ++ch) {
            acc += frameBase[ch];
        }
        out[f] =
            static_cast<int16_t>(std::clamp(acc, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }
}

Result<std::shared_ptr<MonoWavStream>> MonoWavStream::open(const std::string &filePath) {
    if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
        const auto errorMsg = fmt::format("WAV file not found: {}", filePath);
        error(errorMsg);
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::NotFound, errorMsg)};
    }

    auto stream = std::shared_ptr<MonoWavStream>(new MonoWavStream());
    stream->file_.open(filePath, std::ios::binary);
    if (!stream->file_.is_open()) {
        const auto errorMsg = fmt::format("Unable to open WAV file: {}", filePath);
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::Forbidden, errorMsg)};
    }
    std::error_code fileSizeError;
    const auto fileSize = std::filesystem::file_size(filePath, fileSizeError);
    if (fileSizeError) {
        const auto errorMsg =
            fmt::format("Unable to determine WAV file size for '{}': {}", filePath, fileSizeError.message());
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    std::array<uint8_t, 12> riffHeader{};
    if (!readExact(stream->file_, riffHeader.data(), riffHeader.size()) ||
        std::memcmp(riffHeader.data(), "RIFF", 4) != 0 || std::memcmp(riffHeader.data() + 8, "WAVE", 4) != 0) {
        const auto errorMsg = fmt::format("WAV file '{}' has an invalid RIFF/WAVE header", filePath);
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    bool foundFormat = false;
    bool foundData = false;
    std::streampos dataPosition{};
    uint32_t dataSize = 0;

    while (stream->file_ && (!foundFormat || !foundData)) {
        std::array<uint8_t, 8> chunkHeader{};
        if (!readExact(stream->file_, chunkHeader.data(), chunkHeader.size())) {
            break;
        }

        const uint32_t chunkSize = readLittleEndianU32(chunkHeader.data() + 4);
        const bool hasPaddingByte = (chunkSize & 1U) != 0;
        const auto payloadPosition = stream->file_.tellg();
        const auto paddedChunkSize = static_cast<std::uint64_t>(chunkSize) + (hasPaddingByte ? 1ULL : 0ULL);
        if (payloadPosition < 0 || static_cast<std::uint64_t>(payloadPosition) > fileSize ||
            paddedChunkSize > fileSize - static_cast<std::uint64_t>(payloadPosition)) {
            const auto errorMsg = fmt::format("WAV file '{}' declares a chunk beyond the end of the file", filePath);
            return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
        }

        if (std::memcmp(chunkHeader.data(), "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                const auto errorMsg = fmt::format("WAV file '{}' has a truncated format chunk", filePath);
                return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
            }

            const size_t bytesToRead = std::min<size_t>(chunkSize, MAX_FMT_CHUNK_BYTES);
            std::vector<uint8_t> formatData(bytesToRead);
            if (!readExact(stream->file_, formatData.data(), bytesToRead)) {
                const auto errorMsg = fmt::format("WAV file '{}' ended inside its format chunk", filePath);
                return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
            }
            if (chunkSize > bytesToRead) {
                stream->file_.seekg(static_cast<std::streamoff>(chunkSize - bytesToRead), std::ios::cur);
            }

            const uint16_t formatTag = readLittleEndianU16(formatData.data());
            stream->channels_ = readLittleEndianU16(formatData.data() + 2);
            stream->sampleRate_ = static_cast<int>(readLittleEndianU32(formatData.data() + 4));
            const uint32_t byteRate = readLittleEndianU32(formatData.data() + 8);
            stream->blockAlign_ = readLittleEndianU16(formatData.data() + 12);
            const uint16_t bitsPerSample = readLittleEndianU16(formatData.data() + 14);

            const bool isPcm = formatTag == WAVE_FORMAT_PCM ||
                               (formatTag == WAVE_FORMAT_EXTENSIBLE && isPcmExtensibleSubformat(formatData));
            if (!isPcm || bitsPerSample != 16) {
                const auto errorMsg = fmt::format("WAV file '{}' must contain signed 16-bit PCM audio", filePath);
                return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
            }
            if (stream->channels_ == 0 || stream->channels_ > MAX_WAV_CHANNELS ||
                stream->sampleRate_ < MIN_WAV_SAMPLE_RATE || stream->sampleRate_ > MAX_WAV_SAMPLE_RATE) {
                const auto errorMsg = fmt::format("WAV file '{}' has invalid audio dimensions", filePath);
                return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
            }

            const uint32_t expectedBlockAlign = static_cast<uint32_t>(stream->channels_) * sizeof(int16_t);
            const uint64_t expectedByteRate =
                static_cast<uint64_t>(stream->sampleRate_) * static_cast<uint64_t>(expectedBlockAlign);
            if (stream->blockAlign_ != expectedBlockAlign || byteRate != expectedByteRate) {
                const auto errorMsg = fmt::format("WAV file '{}' has inconsistent PCM alignment metadata", filePath);
                return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
            }
            foundFormat = true;
        } else if (std::memcmp(chunkHeader.data(), "data", 4) == 0) {
            dataPosition = stream->file_.tellg();
            dataSize = chunkSize;
            stream->file_.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            foundData = true;
        } else {
            stream->file_.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }

        if (hasPaddingByte) {
            stream->file_.seekg(1, std::ios::cur);
        }
    }

    if (!foundFormat || !foundData) {
        const auto errorMsg = fmt::format("WAV file '{}' is missing its format or data chunk", filePath);
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
    }
    if (dataSize < stream->blockAlign_) {
        const auto errorMsg = fmt::format("WAV file '{}' contains no complete sample frames", filePath);
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    stream->totalFrames_ = dataSize / stream->blockAlign_;
    stream->dataBytesRemaining_ = stream->totalFrames_ * stream->blockAlign_;
    stream->file_.clear();
    stream->file_.seekg(dataPosition);
    if (!stream->file_) {
        const auto errorMsg = fmt::format("Unable to seek to WAV audio data in '{}'", filePath);
        return Result<std::shared_ptr<MonoWavStream>>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    debug("opened streaming WAV '{}': {} channels, {} frames, {} Hz", filePath, stream->channels_, stream->totalFrames_,
          stream->sampleRate_);
    return Result<std::shared_ptr<MonoWavStream>>{stream};
}

Result<size_t> MonoWavStream::readMonoFrames(std::span<int16_t> output) {
    if (output.empty() || dataBytesRemaining_ == 0) {
        return Result<size_t>{0};
    }

    const uint64_t framesRemaining = dataBytesRemaining_ / blockAlign_;
    const size_t framesToRead =
        static_cast<size_t>(std::min<uint64_t>({framesRemaining, output.size(), MAX_STREAM_READ_FRAMES}));
    const size_t bytesToRead = framesToRead * blockAlign_;
    sourceBuffer_.resize(bytesToRead);

    if (!readExact(file_, sourceBuffer_.data(), bytesToRead)) {
        return Result<size_t>{
            ServerError(ServerError::InvalidData, "WAV file ended before the declared audio data was read")};
    }

    for (size_t frame = 0; frame < framesToRead; ++frame) {
        const uint8_t *frameData = sourceBuffer_.data() + frame * blockAlign_;
        int32_t accumulator = 0;
        for (uint16_t channel = 0; channel < channels_; ++channel) {
            const uint8_t *sampleData = frameData + channel * sizeof(int16_t);
            accumulator += static_cast<int16_t>(readLittleEndianU16(sampleData));
        }
        output[frame] = static_cast<int16_t>(
            std::clamp(accumulator, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
    }

    dataBytesRemaining_ -= bytesToRead;
    return Result<size_t>{framesToRead};
}

Result<MonoWav> loadWavAsMono(const std::string &filePath, std::optional<std::uint64_t> maximumFrames) {
    auto streamResult = MonoWavStream::open(filePath);
    if (!streamResult.isSuccess()) {
        return Result<MonoWav>{streamResult.getError().value()};
    }
    const auto stream = streamResult.getValue().value();
    if (maximumFrames && stream->totalFrames() > *maximumFrames) {
        const auto errorMsg = fmt::format("WAV file '{}' has {} frames, exceeding the {}-frame limit", filePath,
                                          stream->totalFrames(), *maximumFrames);
        return Result<MonoWav>{ServerError(ServerError::InvalidData, errorMsg)};
    }
    if (stream->totalFrames() > std::numeric_limits<size_t>::max()) {
        const auto errorMsg = fmt::format("WAV file '{}' is too large to load into memory", filePath);
        return Result<MonoWav>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    MonoWav mono;
    mono.sampleRate = stream->sampleRate();
    mono.samples.resize(static_cast<size_t>(stream->totalFrames()));

    size_t framesRead = 0;
    while (framesRead < mono.samples.size()) {
        constexpr size_t LOAD_CHUNK_FRAMES = 4096;
        const size_t framesRemaining = mono.samples.size() - framesRead;
        const size_t chunkSize = std::min(LOAD_CHUNK_FRAMES, framesRemaining);
        auto readResult = stream->readMonoFrames(std::span<int16_t>{mono.samples}.subspan(framesRead, chunkSize));
        if (!readResult.isSuccess()) {
            return Result<MonoWav>{readResult.getError().value()};
        }
        const size_t chunkFrames = readResult.getValue().value();
        if (chunkFrames == 0) {
            break;
        }
        framesRead += chunkFrames;
    }
    mono.samples.resize(framesRead);

    debug("downmixed '{}' to mono: {} channels, {} frames, {} Hz", filePath, stream->channels(), framesRead,
          stream->sampleRate());
    return Result<MonoWav>{mono};
}

} // namespace creatures::audio
