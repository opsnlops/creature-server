//
// CooperativeAnimationScheduler.cpp
// Modern cooperative playback scheduler implementation
//

#include "CooperativeAnimationScheduler.h"

#include <filesystem>
#include <memory>
#include <thread>

#include "spdlog/spdlog.h"

#include "PlaybackSession.h"
#include "SessionManager.h"
#include "model/PlaylistStatus.h"
#include "server/audio/AudioTransport.h"
#include "server/audio/LocalSdlAudioTransport.h"
#include "server/audio/RtpAudioTransport.h"
#include "server/audio/TravelMonoAudioTransport.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "server/runtime/Activity.h"
#include "server/storage/Storage.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"
#include "util/websocketUtils.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<SessionManager> sessionManager;

// resolveSoundFilePath was a private helper that joined relative paths under
// the permanent sound root. Replaced by creatures::storage::resolveSoundPath
// in the storage facade (issue #11) so every reader of stored sound paths
// uses the same logic.

Result<std::shared_ptr<PlaybackSession>>
CooperativeAnimationScheduler::scheduleAnimation(framenum_t startingFrame, const Animation &animation,
                                                 universe_t universe, creatures::runtime::ActivityReason reason,
                                                 bool cancelEntireUniverse) {
    // Create observability span
    auto scheduleSpan =
        observability ? observability->createOperationSpan("CooperativeAnimationScheduler.scheduleAnimation") : nullptr;
    if (scheduleSpan) {
        scheduleSpan->setAttribute("animation.id", animation.id);
        scheduleSpan->setAttribute("animation.title", animation.metadata.title);
        scheduleSpan->setAttribute("animation.universe", static_cast<int64_t>(universe));
        scheduleSpan->setAttribute("animation.starting_frame", static_cast<int64_t>(startingFrame));
        scheduleSpan->setAttribute("scheduler.type", "cooperative");
        scheduleSpan->setAttribute("adopt.cancel_entire_universe", cancelEntireUniverse);
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

    // A frame decode failure in the constructor leaves the session born-cancelled. Bail
    // before adopting or broadcasting — the old code scheduled a runner for the corpse,
    // which then broadcast (cancelled, stopped) over whatever was actually running (#65).
    if (session->isCancelled()) {
        std::string errorMsg =
            fmt::format("Failed to decode frames for animation '{}'; not scheduling", animation.metadata.title);
        error(errorMsg);
        if (scheduleSpan) {
            scheduleSpan->setError(errorMsg);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    debug("CooperativeAnimationScheduler: scheduling animation '{}' on universe {} at frame {} (session {})",
          animation.metadata.title, universe, startingFrame, session->getSessionId());

    // Adopt the session BEFORE the running broadcast and before the audio load: adoption
    // cancels conflicting sessions and registers this one in a single critical section, so
    // (a) the cancelled sessions' (cancelled, stopped) broadcasts land before our
    // (reason, running) — the ordering the fixture binding dispatcher needs — and (b) the
    // idle-restart check in the playback runner can never observe the universe as free
    // while we're still loading audio (issues #62/#63).
    sessionManager->registerSession(universe, session, false, scheduleSpan, cancelEntireUniverse);

    // Broadcast initial activity state for involved creatures using the session UUID
    const auto &creatureIds = session->getCreatureIds();
    creatures::ws::CreatureService::setActivityRunning(creatureIds, animation.id, reason, session->getSessionId(),
                                                       scheduleSpan);

    // Set up lifecycle callbacks before any runner (sync or async) can fire.
    setupLifecycleCallbacks(session, universe);

    if (!animation.metadata.sound_file.empty()) {
        // Create the audio transport up front (cheap); onStart uses it.
        auto audioTransport = createAudioTransport(session);
        session->setAudioTransport(audioTransport);

        // Only the RTP transport reads the pre-encoded buffer; the local transports
        // load the WAV themselves in onStart.
        if (config && config->getAudioMode() == Configuration::AudioMode::RTP) {
            // The WAV read + 17-channel Opus encode is far too heavy for the event loop
            // thread — which is exactly where playlist events, idle restarts, and queued
            // ad-hoc animations call us from (issue #70). Hand off to a worker; the
            // PlaybackRunnerEvent is only scheduled once the buffer is ready. Safe
            // because adoption already registered the session: every "is anything
            // active?" check sees it for the whole load window.
            scheduleWithAsyncAudioLoad(session, universe, scheduleSpan);

            if (metrics) {
                metrics->incrementAnimationsPlayed();
            }
            if (scheduleSpan) {
                scheduleSpan->setAttribute("audio.load_async", true);
                scheduleSpan->setSuccess();
            }
            info("✅ Scheduled cooperative animation '{}' for universe {} (session {}, audio loading async)",
                 animation.metadata.title, universe, session->getSessionId());
            return Result<std::shared_ptr<PlaybackSession>>{session};
        }

        // Non-RTP transports: nudge the start out the same way the old synchronous path
        // did (+2 frames, plus the configured audio-sync delay).
        startingFrame = eventLoop->getNextFrameNumber() + 2;
        uint32_t delayMs = config->getAnimationDelayMs();
        if (delayMs > 0) {
            framenum_t delayFrames = delayMs / EVENT_LOOP_PERIOD_MS;
            startingFrame += delayFrames;
            debug("Applying animation delay of {}ms ({} frames)", delayMs, delayFrames);
        }
        session->setStartingFrame(startingFrame);
    }

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

void CooperativeAnimationScheduler::scheduleWithAsyncAudioLoad(std::shared_ptr<PlaybackSession> session,
                                                               universe_t universe,
                                                               std::shared_ptr<OperationSpan> scheduleSpan) {
    // Capture shared_ptr copies of the globals so a shutdown mid-load can't yank them
    // out from under the detached worker.
    auto capturedEventLoop = eventLoop;
    auto capturedSessionManager = sessionManager;
    auto capturedConfig = config;
    auto capturedObservability = observability;

    // The schedule span ends when scheduleAnimation returns, so the worker gets its own
    // root span carrying trigger.trace_id/span_id attributes for Honeycomb linkage —
    // same pattern as the fixture AutoStopEvent.
    const std::string triggerTraceId = scheduleSpan ? scheduleSpan->getTraceIdHex() : std::string{};
    const std::string triggerSpanId = scheduleSpan ? scheduleSpan->getSpanIdHex() : std::string{};

    std::thread([session, universe, capturedEventLoop, capturedSessionManager, capturedConfig, capturedObservability,
                 triggerTraceId, triggerSpanId]() {
        auto loadSpan = capturedObservability
                            ? capturedObservability->createOperationSpan("CooperativeAnimationScheduler.asyncAudioLoad")
                            : nullptr;
        if (loadSpan) {
            loadSpan->setAttribute("session.id", session->getSessionId());
            loadSpan->setAttribute("animation.id", session->getAnimation().id);
            loadSpan->setAttribute("session.universe", static_cast<int64_t>(universe));
            if (!triggerTraceId.empty()) {
                loadSpan->setAttribute("trigger.trace_id", triggerTraceId);
                loadSpan->setAttribute("trigger.span_id", triggerSpanId);
            }
        }

        auto loadResult = loadAudioBuffer(session->getAnimation(), session, loadSpan);

        if (!loadResult.isSuccess()) {
            error("Async audio load failed for session {}: {}", session->getSessionId(),
                  loadResult.getError()->getMessage());
            if (loadSpan) {
                loadSpan->setError(loadResult.getError()->getMessage());
                loadSpan->setAttribute("session.aborted_after_adopt", true);
            }

            // Same unwind as any post-adoption failure: the session was adopted and
            // broadcast as running, but no runner will ever fire, so nothing else
            // cleans it up.
            session->cancel();
            session->markCancellationNotified();
            creatures::ws::CreatureService::setActivityState(
                session->getCreatureIds(), session->getAnimation().id, creatures::runtime::ActivityReason::Cancelled,
                creatures::runtime::ActivityState::Stopped, session->getSessionId(), loadSpan);
            if (capturedSessionManager) {
                capturedSessionManager->clearSession(universe, session->getSessionId());
            }
            for (const auto &creatureId : session->getCreatureIds()) {
                creatures::ws::CreatureService::startIdleIfNeeded(creatureId, loadSpan);
            }

            // A playlist can't continue past a broken animation — halt it, matching the
            // old synchronous-failure semantics (and avoiding a retry spin if the
            // weighted pick keeps choosing the same broken file).
            if (session->getActivityReason() == creatures::runtime::ActivityReason::Playlist &&
                capturedSessionManager) {
                warn("Halting playlist on universe {} after audio load failure", universe);
                capturedSessionManager->clearPlaylist(universe);
                PlaylistStatus emptyStatus{};
                emptyStatus.universe = universe;
                emptyStatus.playing = false;
                broadcastPlaylistStatusToAllClients(emptyStatus);
            }
            return;
        }

        if (session->isCancelled()) {
            // Someone adopted over us while we were encoding. The canceller already
            // broadcast our (cancelled, stopped) and scheduled an immediate teardown;
            // just don't start anything.
            debug("Async audio load finished for cancelled session {}; skipping start", session->getSessionId());
            if (loadSpan) {
                loadSpan->setAttribute("session.cancelled_during_load", true);
                loadSpan->setSuccess();
            }
            return;
        }

        if (!capturedEventLoop) {
            warn("Async audio load finished but event loop is gone; dropping session {}", session->getSessionId());
            return;
        }

        framenum_t startFrame = capturedEventLoop->getNextFrameNumber() + 2; // +2 to allow for reset event
        const uint32_t delayMs = capturedConfig ? capturedConfig->getAnimationDelayMs() : 0;
        if (delayMs > 0) {
            startFrame += static_cast<framenum_t>(delayMs / EVENT_LOOP_PERIOD_MS);
            debug("Applying animation delay of {}ms to async start", delayMs);
        }
        session->setStartingFrame(startFrame);

        // Rotate SSRC values one frame before playback so controllers detect the new
        // audio stream.
        auto resetEvent = std::make_shared<RtpEncoderResetEvent>(startFrame - 1, 4); // 4 silent frames
        capturedEventLoop->scheduleEvent(resetEvent);

        auto initialRunner = std::make_shared<PlaybackRunnerEvent>(startFrame, session);
        capturedEventLoop->scheduleEvent(initialRunner);

        info("✅ Async audio ready; animation '{}' starts at frame {} (session {})",
             session->getAnimation().metadata.title, startFrame, session->getSessionId());
        if (loadSpan) {
            loadSpan->setAttribute("session.starting_frame", static_cast<int64_t>(startFrame));
            loadSpan->setSuccess();
        }
    }).detach();
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
    std::filesystem::path soundFilePath = creatures::storage::resolveSoundPath(animation.metadata.sound_file);

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
    }

    if (config->getTravelMode()) {
        debug("Creating travel mono audio transport");
        return std::make_shared<TravelMonoAudioTransport>();
    }

    debug("Creating local SDL audio transport");
    return std::make_shared<LocalSdlAudioTransport>();
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

            // scheduleAnimation adopts (registers) the session itself — see issue #62.
            auto nextResult = CooperativeAnimationScheduler::scheduleAnimation(
                eventLoop->getNextFrameNumber(), *nextQueued, universe, creatures::runtime::ActivityReason::AdHoc);

            if (!nextResult.isSuccess()) {
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
            return;
        }

        // Event-driven playlist chaining (issue #70): the PlaylistEvent pre-scheduled at
        // the *estimated* end frame dies silently if it fires while a session is still
        // active — and with async audio loads the real start (and therefore end) slips
        // past the estimate routinely. Schedule the next link from the actual finish;
        // the estimate-based event remains a harmless backstop, since whichever fires
        // second sees the newly adopted session and skips.
        if (auto finishedSession = weakSession.lock()) {
            if (finishedSession->getActivityReason() == creatures::runtime::ActivityReason::Playlist &&
                sessionManager->getPlaylistState(universe) == PlaylistState::Active && eventLoop) {
                auto nextPlaylistEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
                eventLoop->scheduleEvent(nextPlaylistEvent);
                debug("Playlist chain: scheduled next PlaylistEvent on universe {} after session {} finished", universe,
                      finishedSession->getSessionId());
            }
        }
    });
}

} // namespace creatures
