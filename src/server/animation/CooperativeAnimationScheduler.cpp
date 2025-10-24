//
// CooperativeAnimationScheduler.cpp
// Modern cooperative playback scheduler implementation
//

#include "CooperativeAnimationScheduler.h"

#include <memory>

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
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<universe_t, PlaylistStatus>> runningPlaylists;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<SessionManager> sessionManager;

Result<std::shared_ptr<PlaybackSession>> CooperativeAnimationScheduler::scheduleAnimation(framenum_t startingFrame,
                                                                                          const Animation &animation,
                                                                                          universe_t universe) {
    // Create observability span
    auto scheduleSpan = observability->createOperationSpan("CooperativeAnimationScheduler.scheduleAnimation");
    if (scheduleSpan) {
        scheduleSpan->setAttribute("animation.id", animation.id);
        scheduleSpan->setAttribute("animation.title", animation.metadata.title);
        scheduleSpan->setAttribute("animation.universe", static_cast<int64_t>(universe));
        scheduleSpan->setAttribute("animation.starting_frame", static_cast<int64_t>(startingFrame));
        scheduleSpan->setAttribute("scheduler.type", "cooperative");
    }

    debug("CooperativeAnimationScheduler: scheduling animation '{}' on universe {} at frame {}",
          animation.metadata.title, universe, startingFrame);

    // Create playback session
    auto session = std::make_shared<PlaybackSession>(animation, universe, startingFrame, scheduleSpan);

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
    metrics->incrementAnimationsPlayed();

    if (scheduleSpan) {
        scheduleSpan->setSuccess();
    }

    info("✅ Scheduled cooperative animation '{}' for universe {} starting at frame {}", animation.metadata.title,
         universe, startingFrame);

    return Result<std::shared_ptr<PlaybackSession>>{session};
}

Result<void> CooperativeAnimationScheduler::loadAudioBuffer(const Animation &animation,
                                                            std::shared_ptr<PlaybackSession> session,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto loadSpan = observability->createChildOperationSpan("load_audio_buffer", parentSpan);
    if (loadSpan) {
        loadSpan->setAttribute("sound_file", animation.metadata.sound_file);
    }

    // Build full path to sound file
    std::string soundFilePath = config->getSoundFileLocation() + "/" + animation.metadata.sound_file;

    debug("Loading audio buffer from: {}", soundFilePath);

    // Load and encode audio buffer (heavy I/O operation)
    auto audioBuffer = rtp::AudioStreamBuffer::loadFromWavFile(soundFilePath, loadSpan);
    if (!audioBuffer) {
        std::string errorMsg = fmt::format("Failed to load audio buffer from '{}'", soundFilePath);
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

        auto statusLightOn =
            std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, true);
        eventLoop->scheduleEvent(statusLightOn);

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
        if (auto session = weakSession.lock()) {
            wasCancelled = session->isCancelled();
        }

        if (wasCancelled) {
            // This animation was cancelled (interrupted), don't resume playlists
            // The interrupt animation that replaced this one will handle resume when IT finishes
            debug("PlaybackSession was cancelled, skipping resume logic");
            sessionManager->clearCurrentSession(universe);
            return;
        }

        // Animation finished naturally (not cancelled)
        auto statusLightOff =
            std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, false);
        eventLoop->scheduleEvent(statusLightOff);

        // Clear the session pointer so registerSession() doesn't try to cancel stale sessions
        sessionManager->clearCurrentSession(universe);

        // Check if there's an interrupted playlist that should resume
        if (sessionManager->hasInterruptedPlaylist(universe)) {
            info("Animation finished on universe {} - resuming interrupted playlist", universe);

            // Clear the interrupted state
            sessionManager->resumePlaylist(universe);

            // Schedule a new PlaylistEvent to continue the playlist
            if (runningPlaylists->contains(universe)) {
                auto nextPlaylistEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
                eventLoop->scheduleEvent(nextPlaylistEvent);
                info("Scheduled PlaylistEvent to resume playlist on universe {}", universe);
            } else {
                warn("Interrupted playlist state inconsistent - playlist no longer in cache for universe {}", universe);
            }
        }
    });
}

} // namespace creatures
