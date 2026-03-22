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

    // Create promise/future pair for frame offset synchronization.
    // Sentence N waits for sentence N-1's offset before building.
    {
        std::lock_guard<std::mutex> lock(offsetMutex_);
        offsetPromises_.emplace_back();
        offsetFutures_.push_back(offsetPromises_.back().get_future().share());
    }

    // Kick off full pipeline (TTS + ffmpeg + Opus + animation build) in background.
    // By the time finish() is called, animations are ready to play.
    auto creatureName = creature_.name.empty() ? creatureId_ : creature_.name;
    auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());

    auto future = std::async(std::launch::async,
                              [this, text, sentenceIndex, creatureName, timestamp]() -> Result<Animation> {
                                  auto sentenceSpan = creatures::observability
                                                          ? creatures::observability->createChildOperationSpan(
                                                                fmt::format("StreamingAdHocSession.sentence.{}", sentenceIndex),
                                                                span_)
                                                          : nullptr;
                                  if (sentenceSpan) {
                                      sentenceSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
                                      sentenceSpan->setAttribute("sentence.text", text);
                                  }

                                  // 1. TTS
                                  StreamingTTSClient client;
                                  auto ttsResult = client.generateSpeech(
                                      creatures::config->getVoiceApiKey(), voiceId_, modelId_, text, "mp3_44100_192",
                                      stability_, similarityBoost_, nullptr, sentenceSpan);
                                  if (!ttsResult.isSuccess()) {
                                      if (sentenceSpan) sentenceSpan->setError(ttsResult.getError()->getMessage());
                                      // Unblock next sentence's offset wait
                                      std::lock_guard<std::mutex> lock(offsetMutex_);
                                      if (sentenceIndex <= static_cast<int>(offsetPromises_.size())) {
                                          offsetPromises_[sentenceIndex - 1].set_value(0);
                                      }
                                      return Result<Animation>{ttsResult.getError().value()};
                                  }
                                  const auto tts = ttsResult.getValue().value();

                                  // 2. Write MP3 and convert to WAV
                                  auto tempDir =
                                      std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
                                  std::filesystem::create_directories(tempDir);

                                  auto mp3Path = tempDir / fmt::format("s{}.mp3", sentenceIndex);
                                  {
                                      std::ofstream f(mp3Path, std::ios::binary);
                                      f.write(reinterpret_cast<const char *>(tts.audioData.data()),
                                              static_cast<std::streamsize>(tts.audioData.size()));
                                  }

                                  auto wavPath = tempDir / fmt::format("s{}.wav", sentenceIndex);
                                  auto convertResult = AudioConverter::convertMp3ToWav(
                                      mp3Path, wavPath, creatures::config->getFfmpegBinaryPath(), audioChannel_,
                                      48000, sentenceSpan);
                                  if (!convertResult.isSuccess()) {
                                      if (sentenceSpan) sentenceSpan->setError(convertResult.getError()->getMessage());
                                      std::lock_guard<std::mutex> lock(offsetMutex_);
                                      if (sentenceIndex <= static_cast<int>(offsetPromises_.size())) {
                                          offsetPromises_[sentenceIndex - 1].set_value(0);
                                      }
                                      return Result<Animation>{convertResult.getError().value()};
                                  }

                                  // 3. Opus encoding (parallel across channels)
                                  creatures::rtp::AudioStreamBuffer::loadFromWavFile(wavPath.string(), sentenceSpan);

                                  // 4. Lip sync from alignment
                                  std::vector<RhubarbMouthCue> mouthCues;
                                  if (!tts.charTimings.empty()) {
                                      mouthCues = textToViseme_.charTimingsToMouthCues(tts.charTimings);
                                  }
                                  RhubarbSoundData lipSyncData;
                                  lipSyncData.metadata.soundFile = wavPath.filename().string();
                                  lipSyncData.metadata.duration = tts.audioDurationSeconds;
                                  lipSyncData.mouthCues = mouthCues;

                                  // 5. Wait for previous sentence's frame offset
                                  size_t baseOffset = 0;
                                  if (sentenceIndex > 1) {
                                      baseOffset = offsetFutures_[sentenceIndex - 2].get();
                                  }

                                  // 6. Build animation frames
                                  size_t targetFrames = std::max<size_t>(
                                      1, static_cast<size_t>(std::ceil(
                                             (tts.audioDurationSeconds * 1000.0) / static_cast<double>(msPerFrame_))));

                                  SoundDataProcessor processor;
                                  auto mouthData = processor.processSoundData(lipSyncData, msPerFrame_, targetFrames);
                                  auto mouthSlot = creature_.mouth_slot;

                                  std::vector<std::string> encodedFrames;
                                  encodedFrames.reserve(targetFrames);
                                  for (size_t idx = 0; idx < targetFrames; ++idx) {
                                      auto frameData =
                                          decodedBaseFrames_[(baseOffset + idx) % decodedBaseFrames_.size()];
                                      frameData[mouthSlot] = mouthData[idx];
                                      std::string raw(reinterpret_cast<const char *>(frameData.data()),
                                                      frameData.size());
                                      encodedFrames.push_back(base64::to_base64(raw));
                                  }

                                  // 7. Signal next sentence with our ending offset
                                  size_t endOffset = (baseOffset + targetFrames) % decodedBaseFrames_.size();
                                  {
                                      std::lock_guard<std::mutex> lock(offsetMutex_);
                                      offsetPromises_[sentenceIndex - 1].set_value(endOffset);
                                  }

                                  // 8. Build animation object
                                  auto textSlug = slugify(tts.alignmentText.empty() ? text : tts.alignmentText);
                                  Animation animation = baseAnimation_;
                                  animation.id = util::generateUUID();
                                  animation.metadata.animation_id = animation.id;
                                  animation.metadata.title = fmt::format("{} - {} - s{} - {}", creatureName,
                                                                         timestamp, sentenceIndex, textSlug);
                                  animation.metadata.sound_file = wavPath.string();
                                  animation.metadata.note =
                                      fmt::format("Streaming sentence {}: {}", sentenceIndex, text);
                                  animation.metadata.number_of_frames =
                                      static_cast<uint32_t>(encodedFrames.size());
                                  animation.metadata.multitrack_audio = true;

                                  Track newTrack;
                                  newTrack.id = util::generateUUID();
                                  newTrack.creature_id = creatureId_;
                                  newTrack.animation_id = animation.id;
                                  newTrack.frames = std::move(encodedFrames);
                                  animation.tracks = {newTrack};

                                  // 9. Insert into DB
                                  creatures::db->insertAdHocAnimation(animation,
                                                                       std::chrono::system_clock::now(), sentenceSpan);

                                  if (sentenceSpan) {
                                      sentenceSpan->setAttribute("animation.id", animation.id);
                                      sentenceSpan->setAttribute("animation.frames",
                                                                  static_cast<int64_t>(targetFrames));
                                      sentenceSpan->setAttribute("base_frame_offset",
                                                                  static_cast<int64_t>(baseOffset));
                                      sentenceSpan->setSuccess();
                                  }

                                  info("Sentence {} animation ready: {} frames, offset {}, {:.2f}s",
                                       sentenceIndex, targetFrames, baseOffset, tts.audioDurationSeconds);

                                  return animation;
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

    info("StreamingAdHocSession finishing: session={}, {} sentences, collecting built animations...",
         sessionId_, chunksReceived_);

    // Take ownership of the futures — each contains a fully built Animation
    // (TTS + ffmpeg + Opus + lip sync + frame build all happened in the background)
    std::vector<std::future<Result<Animation>>> futures;
    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        futures = std::move(sentenceFutures_);
        sentenceFutures_.clear();
    }

    if (futures.empty()) {
        return Result<std::string>{ServerError(ServerError::InternalError, "No sentences were submitted")};
    }

    // Write transcript
    auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
    std::filesystem::create_directories(tempDir);
    {
        std::ofstream f(tempDir / "transcript.txt");
        f << fullText_;
    }

    // Collect completed animations IN ORDER and trigger playback.
    // Sentence 1's future.get() should return almost immediately since the
    // background thread started when addText() was called.
    std::string lastAnimationId;
    size_t totalSentences = futures.size();

    for (size_t i = 0; i < totalSentences; ++i) {
        int sentenceIndex = static_cast<int>(i + 1);

        auto animResult = futures[i].get();
        if (!animResult.isSuccess()) {
            warn("Sentence {} failed: {}", sentenceIndex, animResult.getError()->getMessage());
            continue;
        }
        auto animation = animResult.getValue().value();

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
        finishSpan->setAttribute("animations_built", static_cast<int64_t>(totalSentences));
        finishSpan->setSuccess();
    }

    info("StreamingAdHocSession finished: session={}, {} animations, last={}",
         sessionId_, totalSentences, lastAnimationId);

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
