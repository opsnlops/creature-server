//
// CooperativeAnimationScheduler.cpp
// Modern cooperative playback scheduler implementation
//

#include "CooperativeAnimationScheduler.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>

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
#include "server/rtp/AudioLoadExecutor.h"
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
extern std::shared_ptr<rtp::AudioLoadExecutor> rtpAudioLoadExecutor;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<SessionManager> sessionManager;

namespace {

void unwindAdoptedAudioSession(const std::shared_ptr<PlaybackSession> &session, universe_t universe,
                               const std::shared_ptr<SessionManager> &capturedSessionManager,
                               const std::shared_ptr<OperationSpan> &span, bool restartIdle) {
    if (!session) {
        return;
    }

    // No runner will necessarily fire for a loader failure or admission
    // rejection, so perform the complete post-adoption teardown here.
    const bool wasAlreadyCancelled = session->isCancelled();
    session->cancel();
    session->markCancellationNotified();
    try {
        if (auto transport = session->getAudioTransport()) {
            // Releases an RTP output lease if failure happened after acquireOutput.
            transport->stop();
        }
    } catch (const std::exception &ex) {
        error("Audio loader teardown could not stop transport for session {}: {}", session->getSessionId(), ex.what());
    } catch (...) {
        error("Audio loader teardown could not stop transport for session {}", session->getSessionId());
    }

    SessionManager::LoadingSessionAbortResult abortResult;
    try {
        if (capturedSessionManager) {
            abortResult = capturedSessionManager->abortLoadingSession(universe, session->getSessionId());
        } else {
            abortResult.sessionRemoved = !wasAlreadyCancelled;
        }
    } catch (const std::exception &ex) {
        error("Audio loader teardown could not clear session {}: {}", session->getSessionId(), ex.what());
    } catch (...) {
        error("Audio loader teardown could not clear session {}", session->getSessionId());
    }

    // A newer adoption owns the universe and already scheduled this session's
    // teardown. abortLoadingSession checks ownership and clears related state in
    // one critical section, so a late failure cannot erase the replacement's
    // animation queue or playlist.
    if (!abortResult.sessionRemoved) {
        debug("Audio loader failure arrived after session {} was replaced; skipping shared-universe cleanup",
              session->getSessionId());
        return;
    }
    if (abortResult.queuedAnimationsDropped > 0) {
        warn("Audio loader teardown dropped {} queued animation(s) on universe {}", abortResult.queuedAnimationsDropped,
             universe);
    }

    try {
        creatures::ws::CreatureService::setActivityState(
            session->getCreatureIds(), session->getAnimation().id, creatures::runtime::ActivityReason::Cancelled,
            creatures::runtime::ActivityState::Stopped, session->getSessionId(), span,
            session->getActivityGeneration());
    } catch (const std::exception &ex) {
        error("Audio loader teardown could not broadcast stopped state for session {}: {}", session->getSessionId(),
              ex.what());
    } catch (...) {
        error("Audio loader teardown could not broadcast stopped state for session {}", session->getSessionId());
    }

    try {
        if (abortResult.playlistCleared) {
            warn("Halting playlist on universe {} after audio loader failure", universe);
            PlaylistStatus emptyStatus{};
            emptyStatus.universe = universe;
            emptyStatus.playing = false;
            broadcastPlaylistStatusToAllClients(emptyStatus);
        }
    } catch (const std::exception &ex) {
        error("Audio loader teardown could not halt playlist on universe {}: {}", universe, ex.what());
    } catch (...) {
        error("Audio loader teardown could not halt playlist on universe {}", universe);
    }

    // Admission rejection happens precisely because the queue is full. Do not
    // recursively schedule idle audio into the same full queue; the caller gets
    // a synchronous conflict and can retry once capacity becomes available.
    if (restartIdle) {
        for (const auto &creatureId : session->getCreatureIds()) {
            try {
                creatures::ws::CreatureService::startIdleIfNeeded(creatureId, span);
            } catch (const std::exception &ex) {
                error("Audio loader teardown could not restart idle for creature {}: {}", creatureId, ex.what());
            } catch (...) {
                error("Audio loader teardown could not restart idle for creature {}", creatureId);
            }
        }
    }
}

struct AsyncAudioLoadContext {
    std::shared_ptr<OperationSpan> span;
};

} // namespace

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
        // Who asked for this schedule (play / playlist / ad_hoc / idle) — the first
        // GROUP BY when a streaming-conflict refusal or a stall needs explaining.
        scheduleSpan->setAttribute("activity.reason", creatures::runtime::toString(reason));
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

    // Live streaming owns its creatures absolutely — the operator is standing at the
    // creature with a joystick. Every cooperative scheduling path (play, playlist,
    // interrupt, ad-hoc speech, idle) flows through here, so refuse loudly instead of
    // scheduling work the next stream frame would kill with a visible glitch (issue #74).
    for (const auto &track : animation.tracks) {
        if (creatures::ws::CreatureService::isCreatureStreaming(track.creature_id)) {
            std::string errorMsg =
                fmt::format("Creature {} is being live-streamed; refusing to schedule animation '{}'",
                            track.creature_id, animation.metadata.title);
            info("{}", errorMsg);
            if (scheduleSpan) {
                scheduleSpan->setError(errorMsg);
                scheduleSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::Conflict));
                scheduleSpan->setAttribute("streaming.blocked.creature_id", track.creature_id);
                scheduleSpan->setAttribute("streaming.blocked.creature_name",
                                           creatures::ws::CreatureService::resolveCreatureName(track.creature_id));
            }
            return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::Conflict, errorMsg)};
        }
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

    // Fully initialize the session BEFORE adoption publishes it. Once registered, a
    // concurrent adoption can cancel this session and schedule an immediate teardown
    // runner, which reads the callbacks and audio transport on the event-loop thread —
    // setting them after publication was a data race (issue #80).
    setupLifecycleCallbacks(session, universe);

    const bool hasSound = !animation.metadata.sound_file.empty();
    if (hasSound) {
        // Create the audio transport up front (cheap); onStart uses it.
        auto audioTransport = createAudioTransport(session);
        session->setAudioTransport(audioTransport);
    }

    // Adopt the session BEFORE the running broadcast and before the audio load: adoption
    // cancels conflicting sessions and registers this one in a single critical section, so
    // (a) the cancelled sessions' (cancelled, stopped) broadcasts land before our
    // (reason, running) — the ordering the fixture binding dispatcher needs — and (b) the
    // idle-restart check in the playback runner can never observe the universe as free
    // while we're still loading audio (issues #62/#63).
    sessionManager->registerSession(universe, session, false, scheduleSpan, cancelEntireUniverse);

    // Broadcast initial activity state for involved creatures using the session UUID.
    // The generation was minted by adoption just above, so a delayed run of this
    // broadcast can't clobber a newer adoption's state (issue #87).
    const auto &creatureIds = session->getCreatureIds();
    creatures::ws::CreatureService::setActivityRunning(creatureIds, animation.id, reason, session->getSessionId(),
                                                       scheduleSpan, session->getActivityGeneration());

    if (hasSound) {
        // Only the RTP transport reads the pre-encoded buffer; the local transports
        // load the WAV themselves in onStart.
        if (config && config->getAudioMode() == Configuration::AudioMode::RTP) {
            // The WAV read + 17-channel Opus encode is far too heavy for the event loop
            // thread — which is exactly where playlist events, idle restarts, and queued
            // ad-hoc animations call us from (issue #70). Hand off to a worker; the
            // PlaybackRunnerEvent is only scheduled once the buffer is ready. Safe
            // because adoption already registered the session: every "is anything
            // active?" check sees it for the whole load window.
            try {
                auto submitResult = scheduleWithAsyncAudioLoad(session, universe, scheduleSpan);
                if (!submitResult.isSuccess()) {
                    auto submitError = submitResult.getError().value();
                    if (scheduleSpan) {
                        scheduleSpan->setError(submitError.getMessage());
                        scheduleSpan->setAttribute("error.code", static_cast<int64_t>(submitError.getCode()));
                    }
                    return Result<std::shared_ptr<PlaybackSession>>{submitError};
                }
            } catch (const std::exception &ex) {
                const auto errorMsg = fmt::format("RTP audio loader submission failed: {}", ex.what());
                error("{} for session {}", errorMsg, session->getSessionId());
                if (scheduleSpan) {
                    scheduleSpan->recordException(ex);
                    scheduleSpan->setError(errorMsg);
                    scheduleSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                }
                unwindAdoptedAudioSession(session, universe, sessionManager, scheduleSpan, false);
                return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMsg)};
            } catch (...) {
                const std::string errorMsg = "RTP audio loader submission failed with an unknown exception";
                error("{} for session {}", errorMsg, session->getSessionId());
                if (scheduleSpan) {
                    scheduleSpan->setError(errorMsg);
                    scheduleSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                }
                unwindAdoptedAudioSession(session, universe, sessionManager, scheduleSpan, false);
                return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMsg)};
            }

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

Result<void> CooperativeAnimationScheduler::scheduleWithAsyncAudioLoad(std::shared_ptr<PlaybackSession> session,
                                                                       universe_t universe,
                                                                       std::shared_ptr<OperationSpan> scheduleSpan) {
    auto executor = rtpAudioLoadExecutor;
    if (!executor) {
        const std::string errorMsg = "RTP audio loader executor is unavailable";
        error("{} for session {}", errorMsg, session ? session->getSessionId() : std::string{"<missing>"});
        if (scheduleSpan) {
            scheduleSpan->setAttribute("audio.loader.submit_result", "unavailable");
        }
        unwindAdoptedAudioSession(session, universe, sessionManager, scheduleSpan, false);
        return Result<void>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Capture shared_ptr copies of the services so shutdown joins this job
    // before releasing any dependency it may still use.
    auto capturedEventLoop = eventLoop;
    auto capturedSessionManager = sessionManager;
    auto capturedConfig = config;
    auto capturedObservability = observability;
    auto capturedRtpServer = rtpServer;

    // The schedule span ends when scheduleAnimation returns, so the worker gets its own
    // root span carrying trigger.trace_id/span_id attributes for Honeycomb linkage —
    // same pattern as the fixture AutoStopEvent.
    const std::string triggerTraceId = scheduleSpan ? scheduleSpan->getTraceIdHex() : std::string{};
    const std::string triggerSpanId = scheduleSpan ? scheduleSpan->getSpanIdHex() : std::string{};
    const auto queuedAt = std::chrono::steady_clock::now();
    auto loadContext = std::make_shared<AsyncAudioLoadContext>();

    rtp::AudioLoadExecutor::Job job;
    job.id = session->getSessionId();
    job.isCancelled = [session]() { return session->isCancelled(); };
    job.run = [session, universe, capturedEventLoop, capturedConfig, capturedObservability, capturedRtpServer,
               triggerTraceId, triggerSpanId, queuedAt, loadContext]() {
        loadContext->span =
            capturedObservability
                ? capturedObservability->createOperationSpan("CooperativeAnimationScheduler.asyncAudioLoad")
                : nullptr;
        auto loadSpan = loadContext->span;
        if (loadSpan) {
            loadSpan->setAttribute("session.id", session->getSessionId());
            loadSpan->setAttribute("animation.id", session->getAnimation().id);
            loadSpan->setAttribute("session.universe", static_cast<int64_t>(universe));
            loadSpan->setAttribute("audio.loader.queue_wait_ms",
                                   static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::steady_clock::now() - queuedAt)
                                                            .count()));
            if (!triggerTraceId.empty()) {
                loadSpan->setAttribute("trigger.trace_id", triggerTraceId);
                loadSpan->setAttribute("trigger.span_id", triggerSpanId);
            }
        }

        auto loadResult = loadAudioBuffer(session->getAnimation(), session, loadSpan);

        if (!loadResult.isSuccess()) {
            throw std::runtime_error(loadResult.getError()->getMessage());
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
            throw std::runtime_error("Async audio load finished after the event loop became unavailable");
        }

        auto rtpTransport = std::dynamic_pointer_cast<RtpAudioTransport>(session->getAudioTransport());
        if (!capturedRtpServer || !capturedRtpServer->isReady() || !rtpTransport) {
            throw std::runtime_error("Async audio load finished without an available RTP server and transport");
        }

        // Ownership is claimed only after the expensive load finishes. acquireOutput
        // runs on this loader thread, so it can wait for an in-flight 17-channel
        // send without ever delaying the 1ms event loop.
        auto outputLease = capturedRtpServer->acquireOutput("session:" + session->getSessionId());
        rtp::AsyncAudioTraceContext traceContext;
        traceContext.triggerTraceId = triggerTraceId;
        traceContext.triggerSpanId = triggerSpanId;
        traceContext.sessionId = session->getSessionId();
        traceContext.animationId = session->getAnimation().id;
        traceContext.soundFile = session->getAnimation().metadata.sound_file;
        rtpTransport->setOutputLease(outputLease, traceContext);
        if (loadSpan) {
            loadSpan->setAttribute("rtp.generation", static_cast<int64_t>(outputLease.generation));
            loadSpan->setAttribute("rtp.owner_id", outputLease.ownerId);
        }
        if (session->isCancelled()) {
            capturedRtpServer->releaseOutput(outputLease);
            debug("Session {} was cancelled while acquiring RTP output", session->getSessionId());
            if (loadSpan) {
                loadSpan->setAttribute("session.cancelled_during_rtp_acquire", true);
                loadSpan->setSuccess();
            }
            return;
        }

        // Leave a complete 40ms priming window before playback. The reset event
        // schedules one silent RTP frame every 10ms rather than blocking the
        // event loop to send them all at once.
        framenum_t startFrame = capturedEventLoop->getNextFrameNumber() + 2 + RTP_PRIMING_DURATION_FRAMES;
        const uint32_t delayMs = capturedConfig ? capturedConfig->getAnimationDelayMs() : 0;
        if (delayMs > 0) {
            startFrame += static_cast<framenum_t>(delayMs / EVENT_LOOP_PERIOD_MS);
            debug("Applying animation delay of {}ms to async start", delayMs);
        }
        session->setStartingFrame(startFrame);

        // Rotate SSRC values and begin decoder priming immediately before the
        // playback sample timeline.
        auto resetEvent =
            std::make_shared<RtpEncoderResetEvent>(startFrame - RTP_PRIMING_DURATION_FRAMES, outputLease, traceContext);
        capturedEventLoop->scheduleEvent(resetEvent);

        auto initialRunner = std::make_shared<PlaybackRunnerEvent>(startFrame, session);
        capturedEventLoop->scheduleEvent(initialRunner);

        info("✅ Async audio ready; animation '{}' starts at frame {} (session {})",
             session->getAnimation().metadata.title, startFrame, session->getSessionId());
        if (loadSpan) {
            loadSpan->setAttribute("session.starting_frame", static_cast<int64_t>(startFrame));
            loadSpan->setSuccess();
        }
    };
    job.onFailure = [session, universe, capturedSessionManager, capturedObservability, triggerTraceId, triggerSpanId,
                     loadContext](std::exception_ptr failure) {
        auto failureSpan = loadContext->span;
        try {
            if (!failureSpan && capturedObservability) {
                failureSpan =
                    capturedObservability->createOperationSpan("CooperativeAnimationScheduler.asyncAudioLoad.failure");
                if (failureSpan) {
                    failureSpan->setAttribute("session.id", session->getSessionId());
                    failureSpan->setAttribute("animation.id", session->getAnimation().id);
                    failureSpan->setAttribute("session.universe", static_cast<int64_t>(universe));
                    if (!triggerTraceId.empty()) {
                        failureSpan->setAttribute("trigger.trace_id", triggerTraceId);
                        failureSpan->setAttribute("trigger.span_id", triggerSpanId);
                    }
                }
            }
        } catch (const std::exception &ex) {
            error("Could not create async audio failure span for session {}: {}", session->getSessionId(), ex.what());
            failureSpan.reset();
        } catch (...) {
            error("Could not create async audio failure span for session {}", session->getSessionId());
            failureSpan.reset();
        }

        std::string errorMsg = "Async audio load failed with an unknown exception";
        try {
            if (failure) {
                std::rethrow_exception(failure);
            }
        } catch (const std::exception &ex) {
            errorMsg = fmt::format("Async audio load failed: {}", ex.what());
            if (failureSpan) {
                try {
                    failureSpan->recordException(ex);
                    failureSpan->setAttribute("error.type", "std::exception");
                } catch (...) {
                    error("Could not record async audio exception for session {}", session->getSessionId());
                }
            }
        } catch (...) {
            if (failureSpan) {
                try {
                    failureSpan->setAttribute("error.type", "unknown");
                } catch (...) {
                    error("Could not record unknown async audio exception for session {}", session->getSessionId());
                }
            }
        }

        error("{} for session {}", errorMsg, session->getSessionId());
        if (failureSpan) {
            try {
                failureSpan->setError(errorMsg);
                failureSpan->setAttribute("session.aborted_after_adopt", true);
            } catch (...) {
                error("Could not mark async audio failure span for session {}", session->getSessionId());
            }
        }
        unwindAdoptedAudioSession(session, universe, capturedSessionManager, failureSpan, true);
    };

    const auto submitResult = executor->submit(std::move(job));
    const auto stats = executor->getStats();
    if (scheduleSpan) {
        scheduleSpan->setAttribute("audio.loader.workers", static_cast<int64_t>(stats.workerCount));
        scheduleSpan->setAttribute("audio.loader.queue_capacity", static_cast<int64_t>(stats.queueCapacity));
        scheduleSpan->setAttribute("audio.loader.active", static_cast<int64_t>(stats.active));
        scheduleSpan->setAttribute("audio.loader.queued", static_cast<int64_t>(stats.queued));
    }

    if (submitResult == rtp::AudioLoadExecutor::SubmitResult::Accepted) {
        if (scheduleSpan) {
            scheduleSpan->setAttribute("audio.loader.submit_result", "accepted");
        }
        return Result<void>{};
    }

    const bool queueFull = submitResult == rtp::AudioLoadExecutor::SubmitResult::QueueFull;
    const std::string errorMsg = queueFull ? "RTP audio loader queue is full" : "RTP audio loader is shutting down";
    warn("{}; rejecting session {}", errorMsg, session->getSessionId());
    if (scheduleSpan) {
        scheduleSpan->setAttribute("audio.loader.submit_result", queueFull ? "queue_full" : "shutting_down");
    }
    unwindAdoptedAudioSession(session, universe, capturedSessionManager, scheduleSpan, false);
    return Result<void>{ServerError(queueFull ? ServerError::Conflict : ServerError::InternalError, errorMsg)};
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

        // Check if there's an interrupted playlist waiting on our finish. Whether it
        // actually resumes is the interrupt's resumePlaylist choice — the flag used to
        // be write-only and every interrupt auto-resumed (issue #78).
        if (sessionManager->hasInterruptedPlaylist(universe)) {
            if (sessionManager->consumeInterruptResumeDecision(universe)) {
                info("Animation finished on universe {} - resuming interrupted playlist", universe);

                auto snapshot = sessionManager->getPlaylistStatus(universe);
                if (snapshot && !snapshot->playlist.empty()) {
                    if (!eventLoop) {
                        warn("Interrupted playlist resume skipped: event loop unavailable");
                    } else {
                        auto nextPlaylistEvent =
                            std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
                        eventLoop->scheduleEvent(nextPlaylistEvent);
                        info("Scheduled PlaylistEvent to resume playlist on universe {}", universe);
                    }
                } else {
                    warn("Interrupted playlist state inconsistent - playlist snapshot missing for universe {}",
                         universe);
                }
            } else {
                info("Animation finished on universe {} - interrupted playlist stays stopped (resume declined)",
                     universe);
                if (auto snapshot = sessionManager->getPlaylistStatus(universe)) {
                    broadcastPlaylistStatusToAllClients(*snapshot);
                }
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
