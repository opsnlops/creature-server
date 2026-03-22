#include "StreamingAdHocSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include <base64.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "AudioConverter.h"
#include "RhubarbData.h"
#include "SoundDataProcessor.h"
#include "model/Animation.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<util::AudioCache> audioCache;
} // namespace creatures

namespace creatures::voice {

namespace {

std::string slugify(const std::string &value, std::size_t maxLength = 40) {
    std::string slug;
    slug.reserve(std::min<std::size_t>(value.size(), maxLength));
    bool lastDash = false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            lastDash = false;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            if (!lastDash && !slug.empty()) {
                slug.push_back('-');
                lastDash = true;
            }
        }
        if (slug.size() >= maxLength) break;
    }
    if (slug.empty()) slug = "speech";
    if (slug.back() == '-') slug.pop_back();
    return slug;
}

} // namespace

// --- StreamingAdHocSession ---

StreamingAdHocSession::StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId,
                                              bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan)
    : sessionId_(sessionId), creatureId_(creatureId), resumePlaylist_(resumePlaylist) {

    if (parentSpan && creatures::observability) {
        span_ = creatures::observability->createChildOperationSpan("StreamingAdHocSession", parentSpan);
        if (span_) {
            span_->setAttribute("session.id", sessionId);
            span_->setAttribute("creature.id", creatureId);
        }
    }

    info("StreamingAdHocSession created: session={}, creature={}", sessionId, creatureId);
}

StreamingAdHocSession::~StreamingAdHocSession() {
    debug("StreamingAdHocSession destroyed: session={}", sessionId_);
    if (span_) {
        span_->setSuccess();
    }
}

Result<void> StreamingAdHocSession::start() {
    auto startSpan = creatures::observability
                         ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.start", span_)
                         : nullptr;

    // Look up creature
    auto creatureJsonResult = creatures::db->getCreatureJson(creatureId_, startSpan);
    if (!creatureJsonResult.isSuccess()) {
        return Result<void>{creatureJsonResult.getError().value()};
    }
    creatureJson_ = creatureJsonResult.getValue().value();

    auto creatureResult = creatures::db->getCreature(creatureId_, startSpan);
    if (!creatureResult.isSuccess()) {
        return Result<void>{creatureResult.getError().value()};
    }
    creature_ = creatureResult.getValue().value();

    if (!creatureJson_.contains("voice") || creatureJson_["voice"].is_null()) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                         fmt::format("No voice config for creature {}", creatureId_))};
    }

    // Extract voice config
    try {
        audioChannel_ = creatureJson_.value("audio_channel", static_cast<uint16_t>(1));
        auto voiceConfig = creatureJson_["voice"];
        voiceId_ = voiceConfig["voice_id"].get<std::string>();
        modelId_ = voiceConfig["model_id"].get<std::string>();
        stability_ = voiceConfig["stability"].get<float>();
        similarityBoost_ = voiceConfig["similarity_boost"].get<float>();
    } catch (const std::exception &e) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                         fmt::format("Bad voice config: {}", e.what()))};
    }

    // Validate model supports streaming
    static const std::vector<std::string> nonStreamingModels = {"eleven_v3", "eleven_multilingual_v2",
                                                                 "eleven_monolingual_v1", "eleven_multilingual_v1"};
    for (const auto &blocked : nonStreamingModels) {
        if (modelId_ == blocked) {
            return Result<void>{ServerError(
                ServerError::InvalidData,
                fmt::format("Model '{}' does not support WebSocket streaming.", modelId_))};
        }
    }

    // Look up universe for playback
    try {
        auto universePtr = creatures::creatureUniverseMap->get(creatureId_);
        universe_ = *universePtr;
    } catch (const std::exception &e) {
        return Result<void>{ServerError(
            ServerError::InvalidData,
            fmt::format("Creature {} is not registered with a universe.", creatureId_))};
    }

    // Load base animation (reused for all sentences)
    if (creature_.speech_loop_animation_ids.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No speech_loop_animation_ids configured")};
    }

    std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<std::size_t> dist(0, creature_.speech_loop_animation_ids.size() - 1);
    auto baseAnimationId = creature_.speech_loop_animation_ids[dist(rng)];

    auto baseAnimResult = creatures::db->getAnimation(baseAnimationId, startSpan);
    if (!baseAnimResult.isSuccess()) {
        return Result<void>{baseAnimResult.getError().value()};
    }
    baseAnimation_ = baseAnimResult.getValue().value();

    auto trackIt = std::find_if(baseAnimation_.tracks.begin(), baseAnimation_.tracks.end(),
                                [&](const Track &t) { return t.creature_id == creatureId_; });
    if (trackIt == baseAnimation_.tracks.end()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No track for creature in speech loop animation")};
    }

    decodedBaseFrames_.reserve(trackIt->frames.size());
    for (const auto &frame : trackIt->frames) {
        decodedBaseFrames_.push_back(decodeBase64(frame));
    }

    msPerFrame_ = baseAnimation_.metadata.milliseconds_per_frame;
    if (msPerFrame_ == 0) msPerFrame_ = 1;

    // Load CMU dictionary
    auto cmuDictPath = creatures::config->getCmuDictPath();
    if (!cmuDictPath.empty()) {
        textToViseme_.loadCmuDict(cmuDictPath);
    }

    info("StreamingAdHocSession started: session={}, voice={}, model={}, base_anim={} ({} frames)",
         sessionId_, voiceId_, modelId_, baseAnimationId, decodedBaseFrames_.size());

    if (startSpan) {
        startSpan->setAttribute("voice.id", voiceId_);
        startSpan->setAttribute("voice.model", modelId_);
        startSpan->setAttribute("base_animation.id", baseAnimationId);
        startSpan->setAttribute("base_animation.frames", static_cast<int64_t>(decodedBaseFrames_.size()));
        startSpan->setSuccess();
    }

    return Result<void>{};
}

Result<void> StreamingAdHocSession::addText(const std::string &text) {
    if (!fullText_.empty()) {
        fullText_ += " ";
    }
    fullText_ += text;
    chunksReceived_++;

    int sentenceIndex = chunksReceived_;

    info("StreamingAdHocSession sentence {}: \"{}\" ({} chars)", sentenceIndex, text, text.size());

    // Kick off TTS for this sentence immediately in a background thread.
    // Results are collected in finish() and combined into ONE animation.
    auto future = std::async(std::launch::async, [this, text, sentenceIndex]() -> Result<StreamingTTSResult> {
        auto sentenceSpan = creatures::observability
                                ? creatures::observability->createChildOperationSpan(
                                      fmt::format("StreamingAdHocSession.tts.{}", sentenceIndex), span_)
                                : nullptr;
        if (sentenceSpan) {
            sentenceSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
            sentenceSpan->setAttribute("sentence.text", text);
        }

        StreamingTTSClient client;
        auto result = client.generateSpeech(creatures::config->getVoiceApiKey(), voiceId_, modelId_, text,
                                             "mp3_44100_192", stability_, similarityBoost_, nullptr, sentenceSpan);

        if (sentenceSpan) {
            if (result.isSuccess()) {
                const auto &data = result.getValue().value();
                sentenceSpan->setAttribute("audio.bytes", static_cast<int64_t>(data.audioData.size()));
                sentenceSpan->setAttribute("alignment.chars", static_cast<int64_t>(data.charTimings.size()));
                sentenceSpan->setSuccess();
            } else {
                sentenceSpan->setError(result.getError()->getMessage());
            }
        }

        return result;
    });

    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        sentenceFutures_.push_back(std::move(future));
    }

    return Result<void>{};
}

Result<std::string> StreamingAdHocSession::finish() {
    auto finishSpan = creatures::observability
                          ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.finish", span_)
                          : nullptr;

    if (fullText_.empty()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "No text was added to the session")};
    }

    if (finishSpan) {
        finishSpan->setAttribute("text.length", static_cast<int64_t>(fullText_.size()));
        finishSpan->setAttribute("text.sentences", static_cast<int64_t>(chunksReceived_));
    }

    info("StreamingAdHocSession finishing: session={}, {} sentences, collecting TTS results...",
         sessionId_, chunksReceived_);

    // Collect all TTS results in order (most will already be done)
    std::vector<StreamingTTSResult> ttsResults;
    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        for (auto &future : sentenceFutures_) {
            if (future.valid()) {
                auto result = future.get();
                if (result.isSuccess()) {
                    ttsResults.push_back(result.getValue().value());
                } else {
                    warn("Sentence TTS failed: {}", result.getError()->getMessage());
                }
            }
        }
        sentenceFutures_.clear();
    }

    if (ttsResults.empty()) {
        return Result<std::string>{ServerError(ServerError::InternalError, "All sentence TTS calls failed")};
    }

    info("Collected {} TTS results, building per-sentence animations", ttsResults.size());

    // Write transcript
    auto tempRoot = std::filesystem::temp_directory_path() / "creature-adhoc";
    auto tempDir = tempRoot / sessionId_;
    std::filesystem::create_directories(tempDir);
    {
        std::ofstream f(tempDir / "transcript.txt");
        f << fullText_;
    }

    // Process each sentence in order, building a separate animation for each.
    // First sentence triggers interrupt() for immediate playback.
    // Subsequent sentences go to queueAnimation() for seamless chaining.
    auto creatureName = creature_.name.empty() ? creatureId_ : creature_.name;
    auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());
    size_t baseFrameOffset = 0;
    std::string lastAnimationId;

    for (size_t i = 0; i < ttsResults.size(); ++i) {
        const auto &tts = ttsResults[i];
        int sentenceIndex = static_cast<int>(i + 1);

        auto buildSpan = creatures::observability
                             ? creatures::observability->createChildOperationSpan(
                                   fmt::format("StreamingAdHocSession.build.{}", sentenceIndex), finishSpan)
                             : nullptr;

        // Write MP3 and convert to WAV
        auto mp3Path = tempDir / fmt::format("s{}.mp3", sentenceIndex);
        {
            std::ofstream f(mp3Path, std::ios::binary);
            f.write(reinterpret_cast<const char *>(tts.audioData.data()),
                    static_cast<std::streamsize>(tts.audioData.size()));
        }

        auto wavPath = tempDir / fmt::format("s{}.wav", sentenceIndex);
        auto convertResult = AudioConverter::convertMp3ToWav(mp3Path, wavPath,
                                                              creatures::config->getFfmpegBinaryPath(), audioChannel_,
                                                              48000, buildSpan);
        if (!convertResult.isSuccess()) {
            warn("Sentence {} WAV conversion failed: {}", sentenceIndex, convertResult.getError()->getMessage());
            continue;
        }

        // Prewarm audio cache (parallel Opus encoding)
        creatures::rtp::AudioStreamBuffer::loadFromWavFile(wavPath.string(), buildSpan);

        // Generate lip sync
        std::vector<RhubarbMouthCue> mouthCues;
        if (!tts.charTimings.empty()) {
            mouthCues = textToViseme_.charTimingsToMouthCues(tts.charTimings);
        }

        RhubarbSoundData lipSyncData;
        lipSyncData.metadata.soundFile = wavPath.filename().string();
        lipSyncData.metadata.duration = tts.audioDurationSeconds;
        lipSyncData.mouthCues = mouthCues;

        // Build animation frames with correct base frame offset for seamless body motion
        size_t targetFrames = std::max<size_t>(
            1,
            static_cast<size_t>(
                std::ceil((tts.audioDurationSeconds * 1000.0) / static_cast<double>(msPerFrame_))));

        SoundDataProcessor processor;
        auto mouthData = processor.processSoundData(lipSyncData, msPerFrame_, targetFrames);
        auto mouthSlot = creature_.mouth_slot;

        std::vector<std::string> encodedFrames;
        encodedFrames.reserve(targetFrames);
        for (size_t idx = 0; idx < targetFrames; ++idx) {
            auto frameData = decodedBaseFrames_[(baseFrameOffset + idx) % decodedBaseFrames_.size()];
            frameData[mouthSlot] = mouthData[idx];
            std::string raw(reinterpret_cast<const char *>(frameData.data()), frameData.size());
            encodedFrames.push_back(base64::to_base64(raw));
        }

        // Advance offset for next sentence
        baseFrameOffset = (baseFrameOffset + targetFrames) % decodedBaseFrames_.size();

        // Build animation object
        auto textSlug = slugify(tts.alignmentText.empty() ? fullText_ : tts.alignmentText);

        Animation animation = baseAnimation_;
        animation.id = util::generateUUID();
        animation.metadata.animation_id = animation.id;
        animation.metadata.title =
            fmt::format("{} - {} - s{} - {}", creatureName, timestamp, sentenceIndex, textSlug);
        animation.metadata.sound_file = wavPath.string();
        animation.metadata.note = fmt::format("Streaming sentence {}/{}", sentenceIndex, ttsResults.size());
        animation.metadata.number_of_frames = static_cast<uint32_t>(encodedFrames.size());
        animation.metadata.multitrack_audio = true;

        Track newTrack;
        newTrack.id = util::generateUUID();
        newTrack.creature_id = creatureId_;
        newTrack.animation_id = animation.id;
        newTrack.frames = std::move(encodedFrames);
        animation.tracks = {newTrack};

        // Insert into DB
        auto createdAt = std::chrono::system_clock::now();
        creatures::db->insertAdHocAnimation(animation, createdAt, buildSpan);

        if (buildSpan) {
            buildSpan->setAttribute("animation.id", animation.id);
            buildSpan->setAttribute("animation.frames", static_cast<int64_t>(targetFrames));
            buildSpan->setAttribute("base_frame_offset", static_cast<int64_t>(baseFrameOffset));
            buildSpan->setSuccess();
        }

        // First sentence: interrupt for immediate playback
        // Subsequent: queue for seamless chaining
        if (i == 0) {
            info("Sentence {}: interrupt() for immediate playback", sentenceIndex);
            auto sessionResult = creatures::sessionManager->interrupt(universe_, animation, resumePlaylist_);
            if (!sessionResult.isSuccess()) {
                warn("Sentence {} playback failed: {}", sentenceIndex, sessionResult.getError()->getMessage());
            }
        } else {
            info("Sentence {}: queueAnimation() for chained playback", sentenceIndex);
            creatures::sessionManager->queueAnimation(universe_, animation);
        }

        lastAnimationId = animation.id;
    }

    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocAnimationList);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocSoundList);

    if (finishSpan) {
        finishSpan->setAttribute("last_animation.id", lastAnimationId);
        finishSpan->setAttribute("animations_built", static_cast<int64_t>(ttsResults.size()));
        finishSpan->setSuccess();
    }

    info("StreamingAdHocSession finished: session={}, {} animations, last={}",
         sessionId_, ttsResults.size(), lastAnimationId);

    return lastAnimationId;
}

// --- StreamingAdHocSessionManager ---

StreamingAdHocSessionManager &StreamingAdHocSessionManager::instance() {
    static StreamingAdHocSessionManager mgr;
    return mgr;
}

std::shared_ptr<StreamingAdHocSession> StreamingAdHocSessionManager::createSession(
    const std::string &creatureId, bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan) {
    auto sessionId = util::generateUUID();
    auto session =
        std::make_shared<StreamingAdHocSession>(sessionId, creatureId, resumePlaylist, parentSpan);

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[sessionId] = session;
    return session;
}

std::shared_ptr<StreamingAdHocSession> StreamingAdHocSessionManager::getSession(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

void StreamingAdHocSessionManager::removeSession(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

} // namespace creatures::voice
