#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "util/Result.h"

namespace creatures::audio {

class DecodedAudioStream {
  public:
    ~DecodedAudioStream();
    DecodedAudioStream(const DecodedAudioStream &) = delete;
    DecodedAudioStream &operator=(const DecodedAudioStream &) = delete;

    static Result<std::shared_ptr<DecodedAudioStream>> open(const std::string &filePath, uint32_t outputSampleRate,
                                                            uint16_t outputChannels);
    Result<size_t> readFrames(std::span<int16_t> interleavedOutput);
    [[nodiscard]] uint64_t totalFrames() const;
    [[nodiscard]] uint32_t sampleRate() const;
    [[nodiscard]] uint16_t channels() const;

  private:
    struct Impl;
    explicit DecodedAudioStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace creatures::audio
