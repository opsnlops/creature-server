//
// playback-runner.cpp
// Cooperative animation playback runner event
//

#include "types.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include "server/animation/PlaybackSession.h"
#include "server/animation/SessionManager.h"
#include "server/audio/AudioTransport.h"
#include "server/config.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/gpio/gpio.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {

namespace {
constexpr double PLAYBACK_RUNNER_TRACE_SAMPLING = 0.0005; // 0.05% sampling rate

framenum_t frameStepForMs(uint32_t msPerFrame) {
    auto frames = msPerFrame / EVENT_LOOP_PERIOD_MS;
    return std::max<framenum_t>(1, static_cast<framenum_t>(frames));
}
} // namespace

extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<SessionManager> sessionManager;

PlaybackRunnerEvent::PlaybackRunnerEvent(framenum_t frameNumber, std::shared_ptr<PlaybackSession> session)
    : EventBase(frameNumber), session_(session) {}

Result<framenum_t> PlaybackRunnerEvent::executeImpl() {
    if (!session_) {
        std::string errorMsg = "PlaybackRunnerEvent has no session";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!eventLoop) {
        std::string errorMsg = "PlaybackRunnerEvent missing event loop";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    std::shared_ptr<SamplingSpan> runnerSpan = nullptr;
    if (observability) {
        if (auto parentSpan = session_->getSpan()) {
            runnerSpan = observability->createSamplingSpan("playback_runner.execute", parentSpan,
                                                           PLAYBACK_RUNNER_TRACE_SAMPLING);
        } else {
            runnerSpan = observability->createSamplingSpan("playback_runner.execute", PLAYBACK_RUNNER_TRACE_SAMPLING);
        }
    }

    if (runnerSpan) {
        runnerSpan->setAttribute("runner.frame", static_cast<int64_t>(this->frameNumber));
        runnerSpan->setAttribute("session.animation_id", session_->getAnimation().id);
        runnerSpan->setAttribute("session.universe", static_cast<int64_t>(session_->getUniverse()));
        runnerSpan->setAttribute("session.id", session_->getSessionId());
    }

    // Defensive guard: idle sessions should target a single creature.
    if (!session_->isCancelled() && session_->getActivityReason() == creatures::runtime::ActivityReason::Idle) {
        std::unordered_set<creatureId_t> creatureIds;
        for (const auto &trackState : session_->getTrackStates()) {
            if (!trackState.creatureId.empty()) {
                creatureIds.insert(trackState.creatureId);
            }
        }
        if (creatureIds.size() != 1) {
            warn("PlaybackRunnerEvent: idle session {} targets {} creatures; cancelling", session_->getSessionId(),
                 creatureIds.size());
            session_->cancel();
        }
    }

    // Check if session has been cancelled
    if (session_->isCancelled()) {
        debug("PlaybackRunnerEvent detected cancellation, performing teardown");

        // Perform teardown
        performTeardown();

        // Invoke finish callback
        session_->invokeOnFinish();

        // Mark runtime activity as idle for involved creatures
        std::vector<creatureId_t> creatureIds;
        for (const auto &trackState : session_->getTrackStates()) {
            creatureIds.push_back(trackState.creatureId);
        }
        if (!session_->isCancellationNotified()) {
            auto reason = creatures::runtime::ActivityReason::Cancelled;
            creatures::ws::CreatureService::setActivityState(creatureIds, session_->getAnimation().id, reason,
                                                             creatures::runtime::ActivityState::Stopped,
                                                             session_->getSessionId(), session_->getSpan());
        }

        for (const auto &creatureId : creatureIds) {
            ws::CreatureService::startIdleIfNeeded(creatureId, session_->getSpan());
        }

        if (runnerSpan) {
            runnerSpan->setAttribute("runner.cancelled", true);
            runnerSpan->setSuccess();
        }

        return Result{this->frameNumber};
    }

    // Invoke onStart callback on first execution
    session_->invokeOnStart();

    // Emit DMX frames for all tracks at current frame
    if (auto dmxResult = emitDmxFrames(); !dmxResult.isSuccess()) {
        error("Failed to emit DMX frames: {}", dmxResult.getError()->getMessage());
        if (runnerSpan) {
            runnerSpan->setError(dmxResult.getError()->getMessage());
        }
        return dmxResult;
    }

    // Dispatch audio if needed (RTP mode)
    if (auto audioTransport = session_->getAudioTransport();
        audioTransport && audioTransport->needsPerFrameDispatch()) {
        if (auto audioResult = audioTransport->dispatchNextChunk(this->frameNumber); !audioResult.isSuccess()) {
            warn("Audio dispatch failed: {}", audioResult.getError()->getMessage());
            // Non-fatal - continue playback
        }
    }

    // Check if all tracks are finished
    if (areAllTracksFinished()) {
        debug("PlaybackRunnerEvent: all tracks finished, completing playback");

        // Perform teardown
        performTeardown();

        // Invoke finish callback
        session_->invokeOnFinish();

        // Mark runtime activity as idle for involved creatures —
        // but only if the onFinish callback didn't start a new session
        // (e.g., from the animation queue for chained ad-hoc speech)
        std::vector<creatureId_t> creatureIds;
        for (const auto &trackState : session_->getTrackStates()) {
            creatureIds.push_back(trackState.creatureId);
        }

        // Check if a new active non-idle session was started by the callback
        bool newSessionStarted = false;
        if (creatures::sessionManager) {
            for (const auto &cid : creatureIds) {
                if (creatures::sessionManager->hasActiveNonIdleSessionForCreature(
                        session_->getUniverse(), cid)) {
                    newSessionStarted = true;
                    break;
                }
            }
        }

        if (!newSessionStarted) {
            auto reason = session_->getActivityReason();
            ws::CreatureService::setActivityState(creatureIds, session_->getAnimation().id, reason,
                                                  creatures::runtime::ActivityState::Idle, session_->getSessionId(),
                                                  session_->getSpan());

            if (reason != runtime::ActivityReason::Playlist) {
                for (const auto &creatureId : creatureIds) {
                    ws::CreatureService::startIdleIfNeeded(creatureId, session_->getSpan());
                }
            }
        } else {
            debug("PlaybackRunnerEvent: skipping idle — new session was started by onFinish callback");
        }

        if (runnerSpan) {
            runnerSpan->setAttribute("runner.completed_naturally", true);
            runnerSpan->setSuccess();
        }

        return Result{this->frameNumber};
    }

    // Calculate next frame number
    framenum_t nextFrame = calculateNextFrameNumber();

    // Schedule next runner event
    auto nextRunner = std::make_shared<PlaybackRunnerEvent>(nextFrame, session_);
    eventLoop->scheduleEvent(nextRunner);

    if (runnerSpan) {
        runnerSpan->setAttribute("runner.next_frame", static_cast<int64_t>(nextFrame));
        runnerSpan->setSuccess();
    }

    return Result<framenum_t>{this->frameNumber};
}

void PlaybackRunnerEvent::performTeardown() {
    if (!session_) {
        warn("PlaybackRunnerEvent teardown skipped: missing session");
        return;
    }

    debug("PlaybackRunnerEvent performing teardown for animation '{}'", session_->getAnimation().metadata.title);

    // NOTE: We do NOT send DMX blackout - creatures are left in their final state
    // This is intentional to avoid dangerous rapid state changes

    if (!eventLoop) {
        warn("PlaybackRunnerEvent teardown skipped: missing event loop");
    } else {
        // Turn off status light
        auto statusLightOff =
            std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, false);
        eventLoop->scheduleEvent(statusLightOff);
    }

    // Stop audio if playing
    if (const auto audioTransport = session_->getAudioTransport()) {
        audioTransport->stop();
    }

    debug("PlaybackRunnerEvent teardown complete");
}

Result<framenum_t> PlaybackRunnerEvent::emitDmxFrames() {
    trace("emitDmxFrames called for frame {}", this->frameNumber);

    if (!creatureCache) {
        std::string errorMsg = "Creature cache unavailable during playback";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!db) {
        std::string errorMsg = "Database unavailable during playback";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!eventLoop) {
        std::string errorMsg = "Event loop unavailable during playback";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    auto &trackStates = session_->getTrackStates();
    uint32_t framesEmitted = 0;

    // Emit DMX frames for any tracks ready to dispatch on this frame
    for (auto &trackState : trackStates) {
        // Check if this track is finished
        if (trackState.isFinished()) {
            continue;
        }

        // Check if we should dispatch on this frame
        if (this->frameNumber < trackState.nextDispatchFrame) {
            continue; // Not time yet for this track
        }

        // Get the creature for this track
        std::shared_ptr<Creature> creature;

        // First check if it's in the cache
        if (creatureCache->contains(trackState.creatureId)) {
            creature = creatureCache->get(trackState.creatureId);
        } else {
            // Not in cache - fetch from database and cache it
            debug("Creature {} not in cache, fetching from database", trackState.creatureId);
            auto creatureResult = db->getCreature(trackState.creatureId, nullptr);
            if (!creatureResult.isSuccess()) {
                std::string errorMsg =
                    fmt::format("Creature {} not found in database during playback", trackState.creatureId);
                error(errorMsg);
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
            }
            creature = std::make_shared<Creature>(creatureResult.getValue().value());
            creatureCache->put(trackState.creatureId, creature);
            debug("Cached creature {} for playback", trackState.creatureId);
        }

        // Create DMX event for this frame
        auto dmxEvent = std::make_shared<DMXEvent>(this->frameNumber);
        dmxEvent->universe = session_->getUniverse();
        dmxEvent->channelOffset = creature->channel_offset;

        // Copy the decoded frame data
        const auto &frameData = trackState.decodedFrames[trackState.currentFrameIndex];
        dmxEvent->data.reserve(frameData.size());
        for (uint8_t byte : frameData) {
            dmxEvent->data.push_back(byte);
        }

        // Schedule the DMX event
        eventLoop->scheduleEvent(dmxEvent);

        trace("Emitted DMX frame {} for creature {} ({}) on universe {}", trackState.currentFrameIndex, creature->name,
              trackState.creatureId, session_->getUniverse());

        // Advance track state
        trackState.currentFrameIndex++;
        trackState.nextDispatchFrame = this->frameNumber + frameStepForMs(session_->getMsPerFrame());

        framesEmitted++;
    }

    trace("Emitted {} DMX frames for frame {}", framesEmitted, this->frameNumber);
    return Result<framenum_t>{this->frameNumber};
}

bool PlaybackRunnerEvent::areAllTracksFinished() const {
    const auto &trackStates = session_->getTrackStates();

    // If there are no tracks, we're finished
    if (trackStates.empty()) {
        return true;
    }

    // Check if all tracks have finished
    for (const auto &trackState : trackStates) {
        if (!trackState.isFinished()) {
            return false;
        }
    }

    return true;
}

framenum_t PlaybackRunnerEvent::calculateNextFrameNumber() const {
    // Calculate next frame based on ms per frame
    const uint32_t msPerFrame = session_->getMsPerFrame();
    return this->frameNumber + frameStepForMs(msPerFrame);
}

} // namespace creatures
