//
// AudioStreamBuffer.cpp - Fixed to properly handle Result types! 🐰
//

#include <filesystem>

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include "AudioStreamBuffer.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

using namespace creatures;
using namespace creatures::rtp;

std::shared_ptr<AudioStreamBuffer> AudioStreamBuffer::loadFromWav(const std::string &filePath,
                                                                  std::shared_ptr<OperationSpan> parentSpan) {
    auto buf = std::shared_ptr<AudioStreamBuffer>(new AudioStreamBuffer());

    // Now we properly check the Result!
    auto loadResult = buf->loadWave(filePath, std::move(parentSpan));
    if (loadResult.isSuccess()) {
        debug("Successfully loaded audio buffer with {} frames - that's one hoppy bunny! 🐰",
              loadResult.getValue().value_or(0));
        return buf;
    } else {
        error("Failed to load WAV file '{}': {}", filePath, loadResult.getError()->getMessage());
        return nullptr;
    }
}

Result<size_t> AudioStreamBuffer::loadWave(const std::string &path, std::shared_ptr<OperationSpan> parentSpan) {
    const auto span = observability->createChildOperationSpan("AudioStreamBuffer.loadWave", parentSpan);
    span->setAttribute("file_path", path);

    // Early validation - let's make sure this bunny can hop! 🐰
    if (path.empty()) {
        const auto errorMsg = "Empty file path provided - that won't hop very far! 🐰";
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    if (!std::filesystem::exists(path)) {
        const auto errorMsg = fmt::format("WAV file not found: {}", path);
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::NotFound, errorMsg)};
    }

    if (!std::filesystem::is_regular_file(path)) {
        const auto errorMsg = fmt::format("Path is not a regular file: {}", path);
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    SDL_AudioSpec spec{};
    Uint8 *raw = nullptr;
    Uint32 len = 0;

    debug("Loading WAV file: {}", path);

    // SDL_LoadWAV returns nullptr on failure
    if (!SDL_LoadWAV(path.c_str(), &spec, &raw, &len)) {
        const auto errorMsg = fmt::format("SDL failed to load WAV file '{}': {}", path, SDL_GetError());
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // RAII wrapper to ensure SDL memory gets freed
    struct SDLWavGuard {
        Uint8 *data;
        ~SDLWavGuard() {
            if (data)
                SDL_FreeWAV(data);
        }
    } guard{raw};

    // Validate audio format
    if (spec.freq != RTP_SRATE || spec.format != AUDIO_S16 || spec.channels != RTP_STREAMING_CHANNELS) {
        const auto errorMsg =
            fmt::format("WAV file format not supported: {}, {} Hz, {} ch, format 0x{:X} "
                        "(expected {} Hz, {} ch, format 0x{:X})",
                        path, spec.freq, spec.channels, spec.format, RTP_SRATE, RTP_STREAMING_CHANNELS, AUDIO_S16);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Calculate frame counts
    const auto totalSamples = len / sizeof(int16_t);                           // interleaved samples
    framesPerChannel_ = totalSamples / (RTP_STREAMING_CHANNELS * RTP_SAMPLES); // 20ms frames

    if (framesPerChannel_ == 0) {
        const auto errorMsg = fmt::format("WAV file too short: {} ({} samples, need at least {} for one frame)", path,
                                          totalSamples, RTP_STREAMING_CHANNELS * RTP_SAMPLES);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    debug("WAV file loaded: {} total samples, {} frames per channel", totalSamples, framesPerChannel_);

    // Prepare for Opus encoding
    debug("Encoding {} frames to Opus - this bunny is about to work hard! 🐰", framesPerChannel_);

    try {
        // Create encoders for each channel
        std::array<opus::Encoder, RTP_STREAMING_CHANNELS> encoders;

        // Resize storage for all encoded frames
        for (auto &vec : encodedFrames_) {
            vec.resize(framesPerChannel_);
        }

        const int16_t *pcm = reinterpret_cast<const int16_t *>(raw);

        // Encode frame by frame
        for (std::size_t f = 0; f < framesPerChannel_; ++f) {
            const int16_t *frameBase = pcm + f * RTP_SAMPLES * RTP_STREAMING_CHANNELS;

            // Process each channel separately
            for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
                // De-interleave the audio data for this channel
                std::array<int16_t, RTP_SAMPLES> mono{};
                for (std::size_t s = 0; s < RTP_SAMPLES; ++s) {
                    mono[s] = frameBase[s * RTP_STREAMING_CHANNELS + ch];
                }

                // Encode this frame for this channel
                encodedFrames_[ch][f] = encoders[ch].encode(mono.data());
            }

            // Log progress for long files
            if (f % 1000 == 0 && f > 0) {
                debug("Encoded {}/{} frames", f, framesPerChannel_);
            }
        }

        debug("Encoding completed successfully - {} frames per channel encoded to Opus",
              framesPerChannel_);

    } catch (const std::exception &e) {
        const auto errorMsg = fmt::format("Error while encoding WAV to Opus: {}", e.what());
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
    } catch (...) {
        const auto errorMsg = "Unknown error occurred during Opus encoding";
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    info("Successfully loaded and encoded {} frames per channel from WAV file: {}", framesPerChannel_, path);

    span->setSuccess();
    span->setAttribute("frames_per_channel", static_cast<int64_t>(framesPerChannel_));
    span->setAttribute("total_frames", static_cast<int64_t>(framesPerChannel_ * RTP_STREAMING_CHANNELS));

    // Return the number of frames we successfully loaded
    return Result<size_t>{framesPerChannel_};
}