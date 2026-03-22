//
// CooperativeAnimationScheduler.cpp
// Modern cooperative playback scheduler implementation
//

#include "CooperativeAnimationScheduler.h"

#include <filesystem>
#include <memory>
#include <unordered_set>

#include "spdlog/spdlog.h"

#include "PlaybackSession.h"
#include "SessionManager.h"
#include "model/PlaylistStatus.h"
#include "server/audio/AudioTransport.h"
#include "server/audio/LocalSdlAudioTransport.h"
#include "server/audio/RtpAudioTransport.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<SessionManager> sessionManager;

namespace {

std::filesystem::path resolveSoundFilePath(const std::string &soundFile) {
    if (soundFile.empty()) {
        return {};
    }
    std::filesystem::path path(soundFile);
    if (path.is_absolute()) {
        return path;
    }
    return std::filesystem::path(config->getSoundFileLocation()) / path;
}

} // namespace

Result<std::shared_ptr<PlaybackSession>>
CooperativeAnimationScheduler::scheduleAnimation(framenum_t startingFrame, const Animation &animation,
                                                 universe_t universe, creatures::runtime::ActivityReason reason) {
    // Create observability span
    auto scheduleSpan =
        observability ? observability->createOperationSpan("CooperativeAnimationScheduler.scheduleAnimation") : nullptr;
    if (scheduleSpan) {
        scheduleSpan->setAttribute("animation.id", animation.id);
        scheduleSpan->setAttribute("animation.title", animation.metadata.title);
        scheduleSpan->setAttribute("animation.universe", static_cast<int64_t>(universe));
        scheduleSpan->setAttribute("animation.starting_frame", static_cast<int64_t>(startingFrame));
        scheduleSpan->setAttribute("scheduler.type", "cooperative");
    }

    if (!eventLoop) {
        std::string errorMsg = "CooperativeAnimationScheduler: event loop unavailable";
        error(errorMsg);
        if (scheduleSpan) {
            scheduleSpan->setError(errorMsg);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!config) {
        std::string errorMsg = "CooperativeAnimationScheduler: config unavailable";
        error(errorMsg);
        if (scheduleSpan) {
            scheduleSpan->setError(errorMsg);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!sessionManager) {
        std::string errorMsg = "CooperativeAnimationScheduler: session manager unavailable";
        error(errorMsg);
        if (scheduleSpan) {
            scheduleSpan->setError(errorMsg);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (animation.metadata.milliseconds_per_frame == 0 || animation.metadata.number_of_frames == 0) {
        std::string errorMsg =
            fmt::format("Invalid animation timing data for '{}' (ms_per_frame={}, frames={})", animation.metadata.title,
                        animation.metadata.milliseconds_per_frame, animation.metadata.number_of_frames);
        error(errorMsg);
        if (scheduleSpan) {
            scheduleSpan->setError(errorMsg);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Create playback session
    auto session = std::make_shared<PlaybackSession>(animation, universe, startingFrame, scheduleSpan);
    session->setActivityReason(reason);
    if (scheduleSpan) {
        scheduleSpan->setAttribute("session.id", session->getSessionId());
    }

    debug("CooperativeAnimationScheduler: scheduling animation '{}' on universe {} at frame {} (session {})",
          animation.metadata.title, universe, startingFrame, session->getSessionId());

    // Broadcast initial activity state for involved creatures using the session UUID
    std::unordered_set<creatureId_t> creatureSet;
    creatureSet.reserve(animation.tracks.size());
    for (const auto &track : animation.tracks) {
        creatureSet.insert(track.creature_id);
    }
    std::vector<creatureId_t> creatureIds(creatureSet.begin(), creatureSet.end());
    creatures::ws::CreatureService::setActivityRunning(creatureIds, animation.id, reason, session->getSessionId(),
                                                       scheduleSpan);

    // Load audio buffer if animation has sound
    if (!animation.metadata.sound_file.empty()) {
        auto loadResult = loadAudioBuffer(animation, session, scheduleSpan);
        if (!loadResult.isSuccess()) {
            error("Failed to load audio buffer: {}", loadResult.getError()->getMessage());
            if (scheduleSpan) {
                scheduleSpan->setError(loadResult.getError()->getMessage());
            }
            return Result<std::shared_ptr<PlaybackSession>>{loadResult.getError().value()};
        }

        // Create audio transport
        auto audioTransport = createAudioTransport(session);
        session->setAudioTransport(audioTransport);

        // Audio loading is synchronous and heavy I/O - recalculate starting frame
        // This matches the pattern in MusicEvent::scheduleRtpAudio()
        startingFrame = eventLoop->getNextFrameNumber() + 2; // +2 to allow for reset event

        // Apply animation delay for audio sync compensation if configured
        uint32_t delayMs = config->getAnimationDelayMs();
        if (delayMs > 0) {
            framenum_t delayFrames = delayMs / EVENT_LOOP_PERIOD_MS;
            startingFrame += delayFrames;
            debug("Applying animation delay of {}ms ({} frames)", delayMs, delayFrames);
        }

        session->setStartingFrame(startingFrame);

        debug("Audio loaded, adjusted starting frame to {}", startingFrame);

        // If using RTP audio, schedule encoder reset event before playback starts
        // This rotates SSRC values so controllers can detect the new audio stream
        if (config->getAudioMode() == Configuration::AudioMode::RTP) {
            framenum_t resetFrame = startingFrame - 1;
            auto resetEvent = std::make_shared<RtpEncoderResetEvent>(resetFrame, 4); // 4 silent frames
            eventLoop->scheduleEvent(resetEvent);
            debug("Scheduled RtpEncoderResetEvent for frame {} (one frame before animation starts)", resetFrame);
        }
    }

    // Set up lifecycle callbacks
    setupLifecycleCallbacks(session, universe);

    // Schedule the initial PlaybackRunnerEvent
    auto initialRunner = std::make_shared<PlaybackRunnerEvent>(startingFrame, session);
    eventLoop->scheduleEvent(initialRunner);

    debug("CooperativeAnimationScheduler: scheduled initial PlaybackRunnerEvent for frame {}", startingFrame);

    // Increment metrics
    if (metrics) {
        metrics->incrementAnimationsPlayed();
    }

    if (scheduleSpan) {
        scheduleSpan->setSuccess();
    }

    info("✅ Scheduled cooperative animation '{}' for universe {} starting at frame {} (session {})",
         animation.metadata.title, universe, startingFrame, session->getSessionId());

    return Result<std::shared_ptr<PlaybackSession>>{session};
}

Result<void> CooperativeAnimationScheduler::loadAudioBuffer(const Animation &animation,
                                                            std::shared_ptr<PlaybackSession> session,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto loadSpan = observability ? observability->createChildOperationSpan("load_audio_buffer", parentSpan) : nullptr;
    if (loadSpan) {
        loadSpan->setAttribute("sound_file", animation.metadata.sound_file);
    }

    if (!config) {
        std::string errorMsg = "Audio buffer load failed: config unavailable";
        error(errorMsg);
        if (loadSpan) {
            loadSpan->setError(errorMsg);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!session) {
        std::string errorMsg = "Audio buffer load failed: missing playback session";
        error(errorMsg);
        if (loadSpan) {
            loadSpan->setError(errorMsg);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Build full path to sound file
    std::filesystem::path soundFilePath = resolveSoundFilePath(animation.metadata.sound_file);

    debug("Loading audio buffer from: {}", soundFilePath.string());

    // Load and encode audio buffer (heavy I/O operation)
    auto audioBuffer = rtp::AudioStreamBuffer::loadFromWavFile(soundFilePath.string(), loadSpan);
    if (!audioBuffer) {
        std::string errorMsg = fmt::format("Failed to load audio buffer from '{}'", soundFilePath.string());
        error(errorMsg);
        if (loadSpan) {
            loadSpan->setError(errorMsg);
        }
        return Result<void>{ServerError(ServerError::NotFound, errorMsg)};
    }

    session->setAudioBuffer(audioBuffer);

    if (loadSpan) {
        loadSpan->setAttribute("frames_loaded", static_cast<int64_t>(audioBuffer->getFrameCount()));
        loadSpan->setSuccess();
    }

    debug("Audio buffer loaded successfully: {} frames", audioBuffer->getFrameCount());

    return Result<void>{};
}

std::shared_ptr<AudioTransport>
CooperativeAnimationScheduler::createAudioTransport(std::shared_ptr<PlaybackSession> /* session */) {
    if (!config) {
        warn("CooperativeAnimationScheduler: config unavailable for audio transport");
        return nullptr;
    }

    // Check audio mode configuration
    auto audioMode = config->getAudioMode();

    if (audioMode == Configuration::AudioMode::RTP) {
        debug("Creating RTP audio transport");
        return std::make_shared<RtpAudioTransport>(rtpServer);
    } else {
        debug("Creating local SDL audio transport");
        return std::make_shared<LocalSdlAudioTransport>();
    }
}

void CooperativeAnimationScheduler::setupLifecycleCallbacks(std::shared_ptr<PlaybackSession> session,
                                                            universe_t universe) {
    // Use weak_ptr to avoid circular reference
    std::weak_ptr<PlaybackSession> weakSession = session;

    // OnStart callback: turn on status light and start audio
    session->setOnStartCallback([weakSession, universe]() {
        debug("PlaybackSession starting for universe {}", universe);

        if (!eventLoop) {
            warn("PlaybackSession start skipped status light: event loop unavailable");
        } else {
            auto statusLightOn =
                std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, true);
            eventLoop->scheduleEvent(statusLightOn);
        }

        // Start audio transport if present
        if (auto session = weakSession.lock()) {
            auto audioTransport = session->getAudioTransport();
            if (audioTransport) {
                auto startResult = audioTransport->start(session);
                if (!startResult.isSuccess()) {
                    warn("Failed to start audio transport: {}", startResult.getError()->getMessage());
                }
            }
        }
    });

    // OnFinish callback: turn off status light and resume interrupted playlists
    session->setOnFinishCallback([universe, weakSession]() {
        debug("PlaybackSession finishing for universe {}", universe);

        // Check if this session was cancelled vs finished naturally
        bool wasCancelled = false;
        std::string sessionId;
        if (auto session = weakSession.lock()) {
            wasCancelled = session->isCancelled();
            sessionId = session->getSessionId();
        }

        if (!sessionManager) {
            warn("PlaybackSession finish skipped session manager updates: unavailable");
            return;
        }

        if (wasCancelled) {
            // This animation was cancelled (interrupted), don't resume playlists
            // The interrupt animation that replaced this one will handle resume when IT finishes
            debug("PlaybackSession was cancelled, skipping resume logic");
            if (!sessionId.empty()) {
                sessionManager->clearSession(universe, sessionId);
            }
            return;
        }

        // Animation finished naturally (not cancelled)
        if (!eventLoop) {
            warn("PlaybackSession finish skipped status light/playlist scheduling: event loop unavailable");
        } else {
            auto statusLightOff =
                std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, false);
            eventLoop->scheduleEvent(statusLightOff);
        }

        // Clear the session pointer so registerSession() doesn't try to cancel stale sessions
        if (!sessionId.empty()) {
            sessionManager->clearSession(universe, sessionId);
        }

        // Check animation queue before idle/playlist logic.
        // Streaming ad-hoc speech queues sentence animations here for seamless chaining.
        auto nextQueued = sessionManager->popQueuedAnimation(universe);
        if (nextQueued) {
            info("Animation queue: scheduling next animation '{}' on universe {}", nextQueued->metadata.title,
                 universe);

            auto nextResult = CooperativeAnimationScheduler::scheduleAnimation(
                eventLoop->getNextFrameNumber(), *nextQueued, universe, creatures::runtime::ActivityReason::AdHoc);

            if (nextResult.isSuccess()) {
                sessionManager->registerSession(universe, nextResult.getValue().value(), false);
            } else {
                warn("Animation queue: failed to schedule next: {}", nextResult.getError()->getMessage());
            }

            // Don't turn off status light — next animation starts immediately
            // Don't resume playlist — more speech may be queued
            return;
        }

        // Check if there's an interrupted playlist that should resume
        if (sessionManager->hasInterruptedPlaylist(universe)) {
            info("Animation finished on universe {} - resuming interrupted playlist", universe);

            // Clear the interrupted state
            bool resumed = sessionManager->resumePlaylist(universe);
            auto snapshot = sessionManager->getPlaylistStatus(universe);

            // Schedule a new PlaylistEvent to continue the playlist
            if (resumed && snapshot && !snapshot->playlist.empty()) {
                if (!eventLoop) {
                    warn("Interrupted playlist resume skipped: event loop unavailable");
                } else {
                    auto nextPlaylistEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
                    eventLoop->scheduleEvent(nextPlaylistEvent);
                    info("Scheduled PlaylistEvent to resume playlist on universe {}", universe);
                }
            } else {
                warn("Interrupted playlist state inconsistent - playlist snapshot missing for universe {}", universe);
            }
        }
    });
}

} // namespace creatures
