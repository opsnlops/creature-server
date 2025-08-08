//
/**
 * @file AudioStreamBuffer.cpp
 * @brief Implementation of audio stream buffer with Opus encoding and caching
 *
 * This file contains the implementation of the AudioStreamBuffer class which
 * loads WAV files, encodes them to Opus, and provides intelligent caching.
 */
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

// Static cache instance shared across all AudioStreamBuffer instances
std::shared_ptr<util::AudioCache> AudioStreamBuffer::sharedAudioCacheInstance_ = nullptr;

std::shared_ptr<AudioStreamBuffer> AudioStreamBuffer::loadFromWavFile(const std::string &audioFilePath,
                                                                      std::shared_ptr<OperationSpan> parentSpan) {
    auto buf = std::shared_ptr<AudioStreamBuffer>(new AudioStreamBuffer());

    // Try cache-enabled loading first if cache is available
    Result<size_t> loadResult = sharedAudioCacheInstance_
                                    ? buf->loadWithCaching(audioFilePath, parentSpan)
                                    : (debug("No audio cache available, loading directly from WAV file"),
                                       buf->loadWaveFile(audioFilePath, parentSpan));

    if (loadResult.isSuccess()) {
        debug("Successfully loaded audio buffer with {} frames", loadResult.getValue().value_or(0));
        return buf;
    } else {
        error("Failed to load WAV file '{}': {}", audioFilePath, loadResult.getError()->getMessage());
        return nullptr;
    }
}

void AudioStreamBuffer::setAudioCacheInstance(std::shared_ptr<util::AudioCache> audioCacheInstance) {
    sharedAudioCacheInstance_ = audioCacheInstance;
    if (audioCacheInstance) {
        info("Audio cache enabled for AudioStreamBuffer");
    } else {
        info("Audio cache disabled for AudioStreamBuffer");
    }
}

Result<size_t> AudioStreamBuffer::loadWaveFile(const std::string &audioFilePath,
                                               std::shared_ptr<OperationSpan> parentSpan) {
    const auto span = observability->createChildOperationSpan("AudioStreamBuffer.loadWaveFile", parentSpan);
    span->setAttribute("file_path", audioFilePath);

    // Early validation
    if (audioFilePath.empty()) {
        const auto errorMsg = "Empty file path provided";
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    if (!std::filesystem::exists(audioFilePath)) {
        const auto errorMsg = fmt::format("WAV file not found: {}", audioFilePath);
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::NotFound, errorMsg)};
    }

    if (!std::filesystem::is_regular_file(audioFilePath)) {
        const auto errorMsg = fmt::format("Path is not a regular file: {}", audioFilePath);
        span->setError(errorMsg);
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    SDL_AudioSpec spec{};
    Uint8 *raw = nullptr;
    Uint32 len = 0;

    debug("Loading WAV file: {}", audioFilePath);

    // SDL_LoadWAV returns nullptr on failure
    if (!SDL_LoadWAV(audioFilePath.c_str(), &spec, &raw, &len)) {
        const auto errorMsg = fmt::format("SDL failed to load WAV file '{}': {}", audioFilePath, SDL_GetError());
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
        const auto errorMsg = fmt::format("WAV file format not supported: {}, {} Hz, {} ch, format 0x{:X} "
                                          "(expected {} Hz, {} ch, format 0x{:X})",
                                          audioFilePath, spec.freq, spec.channels, spec.format, RTP_SRATE,
                                          RTP_STREAMING_CHANNELS, AUDIO_S16);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Calculate frame counts with overflow protection
    if (len == 0) {
        const auto errorMsg = fmt::format("WAV file has zero length: {}", audioFilePath);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Check for potential overflow in division
    if (len > SIZE_MAX / 2) {
        const auto errorMsg =
            fmt::format("WAV file too large: {} bytes (maximum supported: {} bytes)", len, SIZE_MAX / 2);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    const auto totalSamples = len / sizeof(int16_t);

    // Validate divisor to prevent division by zero
    const auto samplesPerFrame = static_cast<uint64_t>(RTP_STREAMING_CHANNELS) * RTP_SAMPLES;
    if (samplesPerFrame == 0) {
        const auto errorMsg = "Invalid audio configuration: samplesPerFrame is zero";
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Safe division with range checking
    if (totalSamples > SIZE_MAX / samplesPerFrame) {
        const auto errorMsg = fmt::format("WAV file calculation overflow: {} total samples with {} samples per frame",
                                          totalSamples, samplesPerFrame);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    numberOfFramesPerChannel_ = totalSamples / samplesPerFrame;

    if (numberOfFramesPerChannel_ == 0) {
        const auto errorMsg = fmt::format("WAV file too short: {} ({} samples, need at least {} for one frame)",
                                          audioFilePath, totalSamples, samplesPerFrame);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Additional safety check for maximum supported frames
    constexpr size_t MAX_FRAMES_PER_CHANNEL = 1000000; // ~5.5 hours at 48kHz/20ms frames
    if (numberOfFramesPerChannel_ > MAX_FRAMES_PER_CHANNEL) {
        const auto errorMsg = fmt::format("WAV file too long: {} frames per channel (maximum supported: {})",
                                          numberOfFramesPerChannel_, MAX_FRAMES_PER_CHANNEL);
        error(errorMsg);
        span->setError(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    debug("WAV file loaded: {} total samples, {} frames per channel", totalSamples, numberOfFramesPerChannel_);

    // Prepare for Opus encoding
    debug("Encoding {} frames to Opus", numberOfFramesPerChannel_);

    try {
        // Create encoders for each channel
        std::array<opus::Encoder, RTP_STREAMING_CHANNELS> encoders;

        // Resize storage for all encoded frames
        for (auto &frameVector : encodedOpusFrames_) {
            frameVector.resize(numberOfFramesPerChannel_);
        }

        const int16_t *pcm = reinterpret_cast<const int16_t *>(raw);

        // Encode frame by frame
        for (std::size_t frameIndex = 0; frameIndex < numberOfFramesPerChannel_; ++frameIndex) {
            const int16_t *frameBasePointer = pcm + frameIndex * RTP_SAMPLES * RTP_STREAMING_CHANNELS;

            // Process each channel separately
            for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
                // De-interleave the audio data for this channel with bounds checking
                std::array<int16_t, RTP_SAMPLES> monoChannelData{};

                for (std::size_t sampleIndex = 0; sampleIndex < RTP_SAMPLES; ++sampleIndex) {
                    // Calculate the interleaved index with bounds checking
                    const std::size_t interleavedIndex = sampleIndex * RTP_STREAMING_CHANNELS + channelIndex;
                    const std::size_t maxSampleOffset =
                        (numberOfFramesPerChannel_ - frameIndex) * RTP_SAMPLES * RTP_STREAMING_CHANNELS;

                    // Bounds check: ensure we don't read beyond the allocated buffer
                    if (interleavedIndex >= maxSampleOffset) {
                        const auto errorMsg =
                            fmt::format("Buffer overflow prevented: interleavedIndex {} >= maxOffset {} "
                                        "(frame {}/{}, sample {}/{}, channel {})",
                                        interleavedIndex, maxSampleOffset, frameIndex, numberOfFramesPerChannel_,
                                        sampleIndex, RTP_SAMPLES, channelIndex);
                        error(errorMsg);
                        span->setError(errorMsg);
                        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
                    }

                    monoChannelData[sampleIndex] = frameBasePointer[interleavedIndex];
                }

                // Validate channel index before accessing encoders array
                if (channelIndex >= encoders.size()) {
                    const auto errorMsg =
                        fmt::format("Channel index {} exceeds encoder array size {}", channelIndex, encoders.size());
                    error(errorMsg);
                    span->setError(errorMsg);
                    return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
                }

                // Encode this frame for this channel
                encodedOpusFrames_[channelIndex][frameIndex] = encoders[channelIndex].encode(monoChannelData.data());
            }

            // Log progress for long files
            if (frameIndex % 1000 == 0 && frameIndex > 0) {
                debug("Encoded {}/{} frames", frameIndex, numberOfFramesPerChannel_);
            }
        }

        debug("Encoding completed successfully - {} frames per channel encoded to Opus", numberOfFramesPerChannel_);

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

    info("Successfully loaded and encoded {} frames per channel from WAV file: {}", numberOfFramesPerChannel_,
         audioFilePath);

    span->setSuccess();
    span->setAttribute("frames_per_channel", static_cast<int64_t>(numberOfFramesPerChannel_));
    span->setAttribute("total_frames", static_cast<int64_t>(numberOfFramesPerChannel_ * RTP_STREAMING_CHANNELS));

    // Return the number of frames we successfully loaded
    return Result<size_t>{numberOfFramesPerChannel_};
}

Result<size_t> AudioStreamBuffer::loadWithCaching(const std::string &audioFilePath,
                                                  std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability->createChildOperationSpan("AudioStreamBuffer.loadWithCaching", parentSpan);
    span->setAttribute("file_path", audioFilePath);

    // Fast path: try to load from cache
    auto cacheSpan = observability->createChildOperationSpan("AudioStreamBuffer.tryCache", span);
    auto cachedAudioData = sharedAudioCacheInstance_->tryLoadFromCache(audioFilePath, cacheSpan);

    if (cachedAudioData) {
        // Cache hit! Load the data directly
        debug("Cache hit for {}, loading {} frames from cache", audioFilePath, cachedAudioData->framesPerChannel);
        loadFromCachedAudioData(*cachedAudioData);

        span->setAttribute("cache_result", "hit");
        span->setAttribute("frames_loaded", static_cast<int64_t>(numberOfFramesPerChannel_));
        span->setSuccess();

        return Result<size_t>{numberOfFramesPerChannel_};
    }

    // Cache miss: load from WAV and cache the result
    debug("Cache miss for {}, loading from WAV file and caching", audioFilePath);
    span->setAttribute("cache_result", "miss");

    auto loadResult = loadWaveFile(audioFilePath, span);
    if (!loadResult.isSuccess()) {
        span->setError(loadResult.getError()->getMessage());
        return loadResult;
    }

    // Save to cache for next time
    util::AudioCache::CachedAudioData audioDataToCache;
    audioDataToCache.framesPerChannel = numberOfFramesPerChannel_;
    audioDataToCache.encodedFrames = encodedOpusFrames_;

    auto cacheResult = sharedAudioCacheInstance_->saveToCache(audioFilePath, audioDataToCache, span);
    if (cacheResult.isSuccess()) {
        debug("Successfully cached {} frames for {}", numberOfFramesPerChannel_, audioFilePath);
        span->setAttribute("cached_frames", static_cast<int64_t>(numberOfFramesPerChannel_));
    } else {
        warn("Failed to cache audio data for {}: {}", audioFilePath, cacheResult.getError()->getMessage());
        // Don't fail the overall operation if caching fails
    }

    span->setSuccess();
    return loadResult;
}

void AudioStreamBuffer::loadFromCachedAudioData(const util::AudioCache::CachedAudioData &cachedAudioData) {
    numberOfFramesPerChannel_ = cachedAudioData.framesPerChannel;
    encodedOpusFrames_ = cachedAudioData.encodedFrames;

    debug("Loaded {} frames per channel from cached audio data", numberOfFramesPerChannel_);
}