//
/**
 * @file AudioStreamBuffer.cpp
 * @brief Implementation of audio stream buffer with Opus encoding and caching
 *
 * This file contains the implementation of the AudioStreamBuffer class which
 * loads WAV files, encodes them to Opus, and provides intelligent caching.
 */
//

#include <cstdlib>
#include <filesystem>
#include <future>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "AudioStreamBuffer.h"
#include "server/audio/MonoWavDownmixer.h"
#include "server/config/Configuration.h"
#include "server/rtp/opus/OpusPriming.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

using namespace creatures;
using namespace creatures::rtp;

namespace {

std::shared_ptr<std::mutex> getFileLoadMutex(const std::string &audioFilePath) {
    static std::mutex mutexMapMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> mutexes;

    std::error_code error;
    const auto canonicalPath = std::filesystem::weakly_canonical(audioFilePath, error);
    const std::string key = error ? audioFilePath : canonicalPath.string();

    std::lock_guard lock(mutexMapMutex);
    if (const auto existing = mutexes.find(key); existing != mutexes.end()) {
        if (auto mutex = existing->second.lock()) {
            return mutex;
        }
        mutexes.erase(existing);
    }

    auto mutex = std::make_shared<std::mutex>();
    mutexes.emplace(key, mutex);
    return mutex;
}

/**
 * In-memory memo of immutable, fully-loaded buffers (issue #93).
 *
 * Keyed by canonical path with a cheap size+mtime fingerprint. The weak_ptr
 * gives correct sharing for free: concurrent playbacks of one unchanged
 * source get the SAME buffer (every caller already holds its shared_ptr for
 * the whole playback), and an entry dies with its last user. A byte-budgeted
 * strong-ref LRU on top keeps recently played shows warm across playbacks —
 * explicit, bounded retention rather than reloading 17 cache files per play.
 *
 * The per-path load mutex (below) is already held at every access point, so
 * it doubles as the single-flight: no futures plumbing needed.
 */
struct MemoEntry {
    std::uintmax_t fileSize{0};
    std::filesystem::file_time_type modTime{};
    std::weak_ptr<creatures::rtp::AudioStreamBuffer> buffer;
};

std::mutex &memoMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, MemoEntry> &memoEntries() {
    static std::unordered_map<std::string, MemoEntry> entries;
    return entries;
}

std::size_t &memoRetainBudgetBytes() {
    static std::size_t budget = [] {
        if (const char *env = std::getenv(RTP_AUDIO_MEMO_BYTES_ENV)) {
            char *end = nullptr;
            const auto parsed = std::strtoull(env, &end, 10);
            if (end != env && *end == '\0') {
                return static_cast<std::size_t>(parsed);
            }
            spdlog::warn("Ignoring unparseable {}='{}'", RTP_AUDIO_MEMO_BYTES_ENV, env);
        }
        // The travel server is an 8 GB Pi; the main server has 64 GB. Retain
        // accordingly (April's sizing, issue #93).
        if (creatures::config && creatures::config->getTravelMode()) {
            return static_cast<std::size_t>(DEFAULT_RTP_AUDIO_MEMO_BYTES_TRAVEL);
        }
        return static_cast<std::size_t>(DEFAULT_RTP_AUDIO_MEMO_BYTES);
    }();
    return budget;
}

struct RetainedBuffer {
    std::string key;
    std::shared_ptr<creatures::rtp::AudioStreamBuffer> buffer;
};

std::list<RetainedBuffer> &memoLru() {
    static std::list<RetainedBuffer> lru;
    return lru;
}

std::size_t &memoLruBytes() {
    static std::size_t bytes = 0;
    return bytes;
}

// Caller must hold memoMutex(). Moves/creates key at the front and evicts from
// the back until the budget holds (the newest entry always stays, even if it
// alone exceeds the budget — the playback needs it regardless).
void retainInLru(const std::string &key, const std::shared_ptr<creatures::rtp::AudioStreamBuffer> &buffer) {
    auto &lru = memoLru();
    auto &bytes = memoLruBytes();
    for (auto it = lru.begin(); it != lru.end(); ++it) {
        if (it->key == key) {
            bytes -= it->buffer->approximateBytes();
            lru.erase(it);
            break;
        }
    }
    lru.push_front(RetainedBuffer{key, buffer});
    bytes += buffer->approximateBytes();
    while (bytes > memoRetainBudgetBytes() && lru.size() > 1) {
        bytes -= lru.back().buffer->approximateBytes();
        lru.pop_back();
    }
}

std::mutex &encodingJobMutex() {
    // One 17-channel job already launches 17 Opus workers and saturates the
    // production encoder host. Serializing cache misses prevents request bursts
    // from multiplying that CPU load while cache hits remain concurrent.
    static std::mutex mutex;
    return mutex;
}

} // namespace

// Static cache instance shared across all AudioStreamBuffer instances
std::shared_ptr<util::AudioCache> AudioStreamBuffer::sharedAudioCacheInstance_ = nullptr;

std::shared_ptr<AudioStreamBuffer> AudioStreamBuffer::loadFromWavFile(const std::string &audioFilePath,
                                                                      std::shared_ptr<OperationSpan> parentSpan) {
    const auto fileLoadMutex = getFileLoadMutex(audioFilePath);
    std::lock_guard fileLoadLock(*fileLoadMutex);

    std::error_code statError;
    const auto canonicalPath = std::filesystem::weakly_canonical(audioFilePath, statError);
    const std::string memoKey = statError ? audioFilePath : canonicalPath.string();
    const auto currentSize = std::filesystem::file_size(memoKey, statError);
    const auto currentModTime =
        statError ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(memoKey, statError);

    // Memo hit: same unchanged source → share the SAME immutable buffer with
    // every concurrent playback instead of loading another full copy
    // (issue #93). Cheap validation only (size + mtime); a stat change is a
    // plain miss and reloads.
    if (!statError) {
        std::lock_guard memoLock(memoMutex());
        if (auto it = memoEntries().find(memoKey); it != memoEntries().end()) {
            if (it->second.fileSize == currentSize && it->second.modTime == currentModTime) {
                if (auto existing = it->second.buffer.lock()) {
                    debug("Sharing in-memory audio buffer for {} ({} frames)", audioFilePath,
                          existing->getFrameCount());
                    retainInLru(memoKey, existing);
                    return existing;
                }
            }
            memoEntries().erase(it);
        }
    }

    auto buf = std::shared_ptr<AudioStreamBuffer>(new AudioStreamBuffer());

    // Try cache-enabled loading first if cache is available
    Result<size_t> loadResult = sharedAudioCacheInstance_
                                    ? buf->loadWithCaching(audioFilePath, parentSpan)
                                    : (debug("No audio cache available, loading directly from WAV file"),
                                       buf->loadWaveFile(audioFilePath, parentSpan));

    if (loadResult.isSuccess()) {
        debug("Successfully loaded audio buffer with {} frames", loadResult.getValue().value_or(0));
        if (!statError) {
            std::lock_guard memoLock(memoMutex());
            // Opportunistic sweep: one-shot files (each streaming sentence has
            // a unique path) would otherwise leave dead weak entries forever.
            if (memoEntries().size() > 1024) {
                std::erase_if(memoEntries(), [](const auto &entry) { return entry.second.buffer.expired(); });
            }
            memoEntries()[memoKey] = MemoEntry{currentSize, currentModTime, buf};
            retainInLru(memoKey, buf);
        }
        return buf;
    } else {
        error("Failed to load WAV file '{}': {}", audioFilePath, loadResult.getError()->getMessage());
        return nullptr;
    }
}

void AudioStreamBuffer::setMemoRetainBytesForTesting(std::size_t bytes) {
    std::lock_guard memoLock(memoMutex());
    memoRetainBudgetBytes() = bytes;
    memoLru().clear();
    memoLruBytes() = 0;
    memoEntries().clear();
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
    const auto span =
        observability ? observability->createChildOperationSpan("AudioStreamBuffer.loadWaveFile", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("file_path", audioFilePath);
    }

    // Early validation
    if (audioFilePath.empty()) {
        const auto errorMsg = "Empty file path provided";
        if (span) {
            span->setError(errorMsg);
        }
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    if (!std::filesystem::exists(audioFilePath)) {
        const auto errorMsg = fmt::format("WAV file not found: {}", audioFilePath);
        if (span) {
            span->setError(errorMsg);
        }
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::NotFound, errorMsg)};
    }

    if (!std::filesystem::is_regular_file(audioFilePath)) {
        const auto errorMsg = fmt::format("Path is not a regular file: {}", audioFilePath);
        if (span) {
            span->setError(errorMsg);
        }
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    debug("Loading WAV file: {}", audioFilePath);

    auto wavResult = audio::MonoWavStream::open(audioFilePath);
    if (!wavResult.isSuccess()) {
        const auto errorMsg = wavResult.getError()->getMessage();
        if (span) {
            span->setError(errorMsg);
        }
        error(errorMsg);
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }
    const auto wav = wavResult.getValue().value();

    // Validate audio format
    if (wav->sampleRate() != RTP_SRATE || wav->channels() != RTP_STREAMING_CHANNELS) {
        const auto errorMsg =
            fmt::format("WAV file format not supported: {}, {} Hz, {} channels "
                        "(expected {} Hz, {} channels, signed 16-bit PCM)",
                        audioFilePath, wav->sampleRate(), wav->channels(), RTP_SRATE, RTP_STREAMING_CHANNELS);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Calculate frame counts with overflow protection
    if (wav->totalFrames() == 0) {
        const auto errorMsg = fmt::format("WAV file has zero length: {}", audioFilePath);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    if (wav->totalFrames() > SIZE_MAX / RTP_STREAMING_CHANNELS) {
        const auto errorMsg = fmt::format("WAV file sample count overflows address space: {}", audioFilePath);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }
    const auto totalSamples = static_cast<size_t>(wav->totalFrames()) * RTP_STREAMING_CHANNELS;
    // The same duration ceiling the cache reader enforces (issue #93) — the
    // two paths used to disagree by a factor of ~60.
    constexpr size_t MAX_RTP_PCM_SAMPLES =
        RTP_MAX_FRAMES_PER_CHANNEL * static_cast<size_t>(RTP_SAMPLES) * RTP_STREAMING_CHANNELS;
    if (totalSamples > MAX_RTP_PCM_SAMPLES) {
        const auto errorMsg =
            fmt::format("WAV file exceeds the {} second RTP audio ceiling: {}", RTP_MAX_AUDIO_SECONDS, audioFilePath);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Validate divisor to prevent division by zero
    const auto samplesPerFrame = static_cast<uint64_t>(RTP_STREAMING_CHANNELS) * RTP_SAMPLES;
    if (samplesPerFrame == 0) {
        const auto errorMsg = "Invalid audio configuration: samplesPerFrame is zero";
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Safe division with range checking
    if (totalSamples > SIZE_MAX / samplesPerFrame) {
        const auto errorMsg = fmt::format("WAV file calculation overflow: {} total samples with {} samples per frame",
                                          totalSamples, samplesPerFrame);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    numberOfFramesPerChannel_ = totalSamples / samplesPerFrame;

    if (numberOfFramesPerChannel_ == 0) {
        const auto errorMsg = fmt::format("WAV file too short: {} ({} samples, need at least {} for one frame)",
                                          audioFilePath, totalSamples, samplesPerFrame);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Same frame ceiling as the cache-read path (issue #93).
    if (numberOfFramesPerChannel_ > RTP_MAX_FRAMES_PER_CHANNEL) {
        const auto errorMsg = fmt::format("WAV file too long: {} frames per channel (maximum supported: {})",
                                          numberOfFramesPerChannel_, RTP_MAX_FRAMES_PER_CHANNEL);
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    const size_t sampleFramesToEncode = numberOfFramesPerChannel_ * RTP_SAMPLES;
    const size_t samplesToEncode = sampleFramesToEncode * RTP_STREAMING_CHANNELS;
    debug("WAV file loaded: {} declared samples, {} samples used for {} frames per channel", totalSamples,
          samplesToEncode, numberOfFramesPerChannel_);

    std::vector<int16_t> pcmSamples(samplesToEncode);
    size_t sampleFramesRead = 0;
    while (sampleFramesRead < sampleFramesToEncode) {
        const size_t framesRemaining = sampleFramesToEncode - sampleFramesRead;
        const size_t framesRequested = std::min<size_t>(4096, framesRemaining);
        auto readResult = wav->readInterleavedFrames(
            std::span<int16_t>(pcmSamples)
                .subspan(sampleFramesRead * RTP_STREAMING_CHANNELS, framesRequested * RTP_STREAMING_CHANNELS));
        if (!readResult.isSuccess()) {
            if (span) {
                span->setError(readResult.getError()->getMessage());
            }
            return Result<size_t>{readResult.getError().value()};
        }
        const size_t framesRead = readResult.getValue().value();
        if (framesRead == 0) {
            break;
        }
        sampleFramesRead += framesRead;
    }
    if (sampleFramesRead < sampleFramesToEncode) {
        const auto errorMsg = fmt::format("WAV file '{}' ended before its declared sample data", audioFilePath);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Prepare for Opus encoding
    debug("Encoding {} frames to Opus", numberOfFramesPerChannel_);

    try {
        // Resize storage for all encoded frames
        for (auto &frameVector : encodedOpusFrames_) {
            frameVector.resize(numberOfFramesPerChannel_);
        }

        const int16_t *pcm = pcmSamples.data();

        // Encode all 17 channels in parallel — each channel is independent
        // with its own Opus encoder state, so this is trivially parallelizable.
        // On the Ryzen 9 16-core server, all 17 channels encode simultaneously.
        std::array<std::future<Result<size_t>>, RTP_STREAMING_CHANNELS> futures;

        for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            futures[channelIndex] = std::async(std::launch::async, [this, pcm, channelIndex, span]() -> Result<size_t> {
                auto channelSpan = observability
                                       ? observability->createChildOperationSpan(
                                             fmt::format("AudioStreamBuffer.encodeChannel.{}", channelIndex), span)
                                       : nullptr;
                if (channelSpan) {
                    channelSpan->setAttribute("channel", static_cast<int64_t>(channelIndex));
                    channelSpan->setAttribute("frames", static_cast<int64_t>(numberOfFramesPerChannel_));
                }

                opus::Encoder encoder;
                // Match the decoder history established by
                // MultiOpusRtpServer's startup silence sequence.
                static_cast<void>(opus::encodePrimingSequence(encoder));

                for (std::size_t frameIndex = 0; frameIndex < numberOfFramesPerChannel_; ++frameIndex) {
                    const int16_t *frameBase = pcm + frameIndex * RTP_SAMPLES * RTP_STREAMING_CHANNELS;

                    // De-interleave this channel's samples from the interleaved PCM
                    std::array<int16_t, RTP_SAMPLES> mono{};
                    for (std::size_t s = 0; s < RTP_SAMPLES; ++s) {
                        mono[s] = frameBase[s * RTP_STREAMING_CHANNELS + channelIndex];
                    }

                    encodedOpusFrames_[channelIndex][frameIndex] = encoder.encode(mono.data());
                }

                if (channelSpan) {
                    channelSpan->setSuccess();
                }
                return Result<size_t>{numberOfFramesPerChannel_};
            });
        }

        // Wait for all channels to complete
        for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            auto channelResult = futures[channelIndex].get();
            if (!channelResult.isSuccess()) {
                auto errorMsg = fmt::format("Opus encoding failed for channel {}: {}", channelIndex,
                                            channelResult.getError()->getMessage());
                error(errorMsg);
                if (span) {
                    span->setError(errorMsg);
                }
                return Result<size_t>{channelResult.getError().value()};
            }
        }

        debug("Parallel encoding completed - {} frames × {} channels encoded to Opus", numberOfFramesPerChannel_,
              RTP_STREAMING_CHANNELS);

    } catch (const std::exception &e) {
        const auto errorMsg = fmt::format("Error while encoding WAV to Opus: {}", e.what());
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
    } catch (...) {
        const auto errorMsg = "Unknown error occurred during Opus encoding";
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<size_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    computeApproximateBytes();

    info("Successfully loaded and encoded {} frames per channel from WAV file: {}", numberOfFramesPerChannel_,
         audioFilePath);

    if (span) {
        span->setSuccess();
    }
    if (span) {
        span->setAttribute("frames_per_channel", static_cast<int64_t>(numberOfFramesPerChannel_));
    }
    if (span) {
        span->setAttribute("total_frames", static_cast<int64_t>(numberOfFramesPerChannel_ * RTP_STREAMING_CHANNELS));
    }

    // Return the number of frames we successfully loaded
    return Result<size_t>{numberOfFramesPerChannel_};
}

Result<size_t> AudioStreamBuffer::loadWithCaching(const std::string &audioFilePath,
                                                  std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability ? observability->createChildOperationSpan("AudioStreamBuffer.loadWithCaching", parentSpan)
                              : nullptr;
    if (span) {
        span->setAttribute("file_path", audioFilePath);
    }

    // Fast path: try to load from cache
    auto cacheSpan =
        observability ? observability->createChildOperationSpan("AudioStreamBuffer.tryCache", span) : nullptr;

    auto cachedAudioData = sharedAudioCacheInstance_->tryLoadFromCache(audioFilePath, cacheSpan);

    if (cachedAudioData) {
        // Cache hit! Take ownership of the packets — the old copy here doubled
        // peak memory on every hit (issue #93).
        debug("Cache hit for {}, loading {} frames from cache", audioFilePath, cachedAudioData->framesPerChannel);
        loadFromCachedAudioData(std::move(*cachedAudioData));

        if (span) {
            span->setAttribute("cache_result", "hit");
        }
        if (span) {
            span->setAttribute("frames_loaded", static_cast<int64_t>(numberOfFramesPerChannel_));
        }
        if (span) {
            span->setSuccess();
        }

        return Result<size_t>{numberOfFramesPerChannel_};
    }

    // Cache miss: load from WAV and cache the result
    debug("Cache miss for {}, loading from WAV file and caching", audioFilePath);
    if (span) {
        span->setAttribute("cache_result", "miss");
    }

    std::lock_guard encodingJobLock(encodingJobMutex());

    // Fingerprint the source BEFORE the WAV is opened (issue #93): the
    // verifying save below refuses to publish if the file changed during the
    // encode, so old packets can never be labeled with a replacement file's
    // identity.
    auto expectedSourceInfo = sharedAudioCacheInstance_->getSourceFileInfo(audioFilePath);

    auto loadResult = loadWaveFile(audioFilePath, span);
    if (!loadResult.isSuccess()) {
        if (span) {
            span->setError(loadResult.getError()->getMessage());
        }
        return loadResult;
    }

    if (!expectedSourceInfo.isSuccess()) {
        // Couldn't fingerprint before the read: play the audio, skip caching.
        warn("Skipping audio cache save for {}: {}", audioFilePath, expectedSourceInfo.getError()->getMessage());
        if (span) {
            span->setAttribute("cache_result", "miss_uncached");
            span->setSuccess();
        }
        return loadResult;
    }

    // Save to cache for next time — frames passed by reference (the old copy
    // into a temporary struct tripled peak memory on a miss, issue #93).
    auto cacheResult = sharedAudioCacheInstance_->saveToCache(
        audioFilePath, numberOfFramesPerChannel_, encodedOpusFrames_, expectedSourceInfo.getValue().value(), span);
    if (cacheResult.isSuccess()) {
        debug("Successfully cached {} frames for {}", numberOfFramesPerChannel_, audioFilePath);
        if (span) {
            span->setAttribute("cached_frames", static_cast<int64_t>(numberOfFramesPerChannel_));
        }
    } else {
        warn("Failed to cache audio data for {}: {}", audioFilePath, cacheResult.getError()->getMessage());
        // Don't fail the overall operation if caching fails
    }

    if (span) {
        span->setSuccess();
    }
    return loadResult;
}

void AudioStreamBuffer::loadFromCachedAudioData(util::AudioCache::CachedAudioData &&cachedAudioData) {
    numberOfFramesPerChannel_ = cachedAudioData.framesPerChannel;
    encodedOpusFrames_ = std::move(cachedAudioData.encodedFrames);
    computeApproximateBytes();

    debug("Loaded {} frames per channel from cached audio data", numberOfFramesPerChannel_);
}

void AudioStreamBuffer::computeApproximateBytes() {
    std::size_t bytes = 0;
    for (const auto &channel : encodedOpusFrames_) {
        for (const auto &frame : channel) {
            bytes += frame.size() + RTP_ENCODED_FRAME_OVERHEAD_BYTES;
        }
    }
    approximateBytes_ = bytes;
}
