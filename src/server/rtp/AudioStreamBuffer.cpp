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

/// Canonical form of a path, used as the key for BOTH the per-path load mutex
/// and the memo so the two can never disagree about identity (issue #93).
std::string canonicalKeyFor(const std::string &audioFilePath) {
    std::error_code error;
    const auto canonicalPath = std::filesystem::weakly_canonical(audioFilePath, error);
    return error ? audioFilePath : canonicalPath.string();
}

std::shared_ptr<std::mutex> getFileLoadMutex(const std::string &canonicalKey) {
    static std::mutex mutexMapMutex;
    static std::unordered_map<std::string, std::weak_ptr<std::mutex>> mutexes;

    std::lock_guard lock(mutexMapMutex);
    if (const auto existing = mutexes.find(canonicalKey); existing != mutexes.end()) {
        if (auto mutex = existing->second.lock()) {
            return mutex;
        }
        mutexes.erase(existing);
    }

    auto mutex = std::make_shared<std::mutex>();
    mutexes.emplace(canonicalKey, mutex);
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
 * The per-path load mutex is already held at every access point, so it also
 * serves as the single-flight: no futures plumbing needed.
 *
 * **Staleness contract.** The fingerprint is size + mtime, NOT content. The
 * disk cache underneath validates by SHA-256, but hashing a show-length WAV
 * costs seconds and cannot sit on playback start, so a memo hit deliberately
 * skips it. A file replaced in place with BOTH the same byte length and a
 * preserved mtime (`cp -p` / `rsync -a` of a same-duration re-render) would
 * therefore serve the previous audio until the entry is evicted or
 * invalidated. Every server-mediated write funnels through the storage
 * facade, which calls invalidate() — the residual exposure is out-of-band
 * timestamp-preserving replacement, for which the debug cache-invalidate
 * endpoints are the operator's lever.
 */
class BufferMemo {
  public:
    /// Buffers whose strong references were dropped by this call. The caller
    /// destroys them AFTER releasing the lock: freeing a show-sized buffer is
    /// ~1.5M vector deallocations, which must not stall concurrent memo hits.
    using Evicted = std::vector<std::shared_ptr<creatures::rtp::AudioStreamBuffer>>;

    [[nodiscard]] std::shared_ptr<creatures::rtp::AudioStreamBuffer>
    lookup(const std::string &key, std::uintmax_t fileSize, std::filesystem::file_time_type modTime, Evicted &evicted) {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            return nullptr;
        }
        if (it->second.fileSize == fileSize && it->second.modTime == modTime) {
            if (auto existing = it->second.buffer.lock()) {
                retainLocked(key, existing, evicted);
                return existing;
            }
        }
        entries_.erase(it);
        return nullptr;
    }

    /**
     * Publish a freshly loaded buffer.
     *
     * @param retain false for one-shot loads (prewarms whose caller discards
     *               the buffer): the weak entry still enables sharing with a
     *               concurrent load, but the byte budget is reserved for audio
     *               that actually plays again, so a long ad-hoc speech session
     *               cannot evict warm show audio with dead sentences.
     */
    void store(const std::string &key, std::uintmax_t fileSize, std::filesystem::file_time_type modTime,
               const std::shared_ptr<creatures::rtp::AudioStreamBuffer> &buffer, bool retain, Evicted &evicted) {
        std::lock_guard lock(mutex_);
        // One-shot files (each streaming sentence gets a unique path) would
        // otherwise leave dead weak entries behind forever.
        if (entries_.size() > MAX_WEAK_ENTRIES) {
            std::erase_if(entries_, [](const auto &entry) { return entry.second.buffer.expired(); });
        }
        entries_[key] = MemoEntry{fileSize, modTime, buffer};
        if (retain) {
            retainLocked(key, buffer, evicted);
        }
    }

    void invalidate(const std::string &key, Evicted &evicted) {
        std::lock_guard lock(mutex_);
        entries_.erase(key);
        dropRetainedLocked(key, evicted);
    }

    void clear(Evicted &evicted) {
        std::lock_guard lock(mutex_);
        entries_.clear();
        for (auto &retained : lru_) {
            evicted.push_back(std::move(retained.buffer));
        }
        lru_.clear();
        lruBytes_ = 0;
    }

    void setBudgetBytes(std::size_t bytes, Evicted &evicted) {
        std::lock_guard lock(mutex_);
        budgetBytes_ = bytes;
        evictToBudgetLocked(evicted);
    }

    [[nodiscard]] std::size_t budgetBytes() {
        std::lock_guard lock(mutex_);
        return budgetBytes_;
    }

    [[nodiscard]] std::size_t retainedBytes() {
        std::lock_guard lock(mutex_);
        return lruBytes_;
    }

  private:
    struct MemoEntry {
        std::uintmax_t fileSize{0};
        std::filesystem::file_time_type modTime{};
        std::weak_ptr<creatures::rtp::AudioStreamBuffer> buffer;
    };

    struct RetainedBuffer {
        std::string key;
        std::shared_ptr<creatures::rtp::AudioStreamBuffer> buffer;
    };

    static constexpr std::size_t MAX_WEAK_ENTRIES = 1024;

    void dropRetainedLocked(const std::string &key, Evicted &evicted) {
        for (auto it = lru_.begin(); it != lru_.end(); ++it) {
            if (it->key == key) {
                lruBytes_ -= it->buffer->approximateBytes();
                evicted.push_back(std::move(it->buffer));
                lru_.erase(it);
                return;
            }
        }
    }

    void retainLocked(const std::string &key, const std::shared_ptr<creatures::rtp::AudioStreamBuffer> &buffer,
                      Evicted &evicted) {
        dropRetainedLocked(key, evicted);
        lru_.push_front(RetainedBuffer{key, buffer});
        lruBytes_ += buffer->approximateBytes();
        evictToBudgetLocked(evicted);
    }

    // The newest entry always stays, even if it alone exceeds the budget — the
    // playback that just loaded it needs it regardless.
    void evictToBudgetLocked(Evicted &evicted) {
        while (lruBytes_ > budgetBytes_ && lru_.size() > 1) {
            lruBytes_ -= lru_.back().buffer->approximateBytes();
            evicted.push_back(std::move(lru_.back().buffer));
            lru_.pop_back();
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, MemoEntry> entries_;
    std::list<RetainedBuffer> lru_;
    std::size_t lruBytes_{0};
    // Fail safe until main.cpp injects the resolved value: the SMALL default,
    // so a load that happens before configuration (or in tests) can never latch
    // the 64 GB server's budget onto the 8 GB travel Pi (issue #93 review).
    std::size_t budgetBytes_{DEFAULT_RTP_AUDIO_MEMO_BYTES_TRAVEL};
};

BufferMemo &bufferMemo() {
    static BufferMemo memo;
    return memo;
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
                                                                      std::shared_ptr<OperationSpan> parentSpan,
                                                                      RetentionIntent retention) {
    // One canonicalization shared by the load mutex and the memo, so the two
    // can never key the same file differently (issue #93 review).
    const auto canonicalKey = canonicalKeyFor(audioFilePath);
    const auto fileLoadMutex = getFileLoadMutex(canonicalKey);
    std::lock_guard fileLoadLock(*fileLoadMutex);

    // Evicted buffers destruct here, at end of scope — after every memo lock
    // has been released (issue #93 review).
    BufferMemo::Evicted evicted;

    std::error_code sizeError;
    std::error_code modTimeError;
    const auto currentSize = std::filesystem::file_size(canonicalKey, sizeError);
    const auto currentModTime = std::filesystem::last_write_time(canonicalKey, modTimeError);
    const bool fingerprinted = !sizeError && !modTimeError;

    // Memo hit: same unchanged source → share the SAME immutable buffer with
    // every concurrent playback instead of loading another full copy
    // (issue #93). See BufferMemo's staleness contract for what size+mtime
    // does and does not catch.
    if (fingerprinted) {
        if (auto existing = bufferMemo().lookup(canonicalKey, currentSize, currentModTime, evicted)) {
            debug("Sharing in-memory audio buffer for {} ({} frames)", audioFilePath, existing->getFrameCount());
            return existing;
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
        if (fingerprinted) {
            bufferMemo().store(canonicalKey, currentSize, currentModTime, buf, retention == RetentionIntent::Retain,
                               evicted);
        }
        return buf;
    } else {
        error("Failed to load WAV file '{}': {}", audioFilePath, loadResult.getError()->getMessage());
        return nullptr;
    }
}

void AudioStreamBuffer::setMemoRetainBytes(std::size_t bytes) {
    BufferMemo::Evicted evicted;
    bufferMemo().setBudgetBytes(bytes, evicted);
    info("In-memory audio buffer retention budget set to {} bytes", bytes);
}

std::size_t AudioStreamBuffer::memoRetainBytes() { return bufferMemo().budgetBytes(); }

std::size_t AudioStreamBuffer::memoRetainedBytes() { return bufferMemo().retainedBytes(); }

void AudioStreamBuffer::invalidateMemo(const std::string &audioFilePath) {
    BufferMemo::Evicted evicted;
    bufferMemo().invalidate(canonicalKeyFor(audioFilePath), evicted);
}

void AudioStreamBuffer::clearMemo() {
    BufferMemo::Evicted evicted;
    bufferMemo().clear(evicted);
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

    // Fingerprint the source BEFORE the WAV is opened (issue #93): the
    // verifying save below refuses to publish if the file changed during the
    // encode, so old packets can never be labeled with a replacement file's
    // identity.
    //
    // Deliberately hashed OUTSIDE encodingJobMutex (issue #93 review): that
    // mutex throttles encoder CPU, and a show-length source is a multi-second
    // SHA-256 read that would otherwise serialize behind every other pending
    // cache miss. The TOCTOU window is unchanged — what matters is that the
    // fingerprint predates the WAV read, not which lock is held.
    auto expectedSourceInfo = sharedAudioCacheInstance_->getSourceFileInfo(audioFilePath);

    std::lock_guard encodingJobLock(encodingJobMutex());

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
    approximateBytes_ = cachedAudioData.approximateBytes;
    encodedOpusFrames_ = std::move(cachedAudioData.encodedFrames);
    if (approximateBytes_ == 0) {
        // Pre-#93 cache reader, or an empty load: fall back to walking.
        computeApproximateBytes();
    }

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
