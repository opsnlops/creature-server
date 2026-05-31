#include "PcmWavWriter.h"

#include <fmt/format.h>
#include <fstream>

#include "server/config.h"
#include "server/namespace-stuffs.h"

namespace creatures::voice {

Result<std::size_t> writePcmToMultichannelWav(const std::vector<uint8_t> &pcmData,
                                              const std::filesystem::path &wavPath, uint16_t audioChannel,
                                              uint32_t sampleRate) {
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

} // namespace creatures::voice
