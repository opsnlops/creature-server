//
// playback-runner.cpp
// Cooperative animation playback runner event
//

#include "types.h"

#include <memory>

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include "server/animation/PlaybackSession.h"
#include "server/audio/AudioTransport.h"
#include "server/config.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {

namespace {
constexpr double PLAYBACK_RUNNER_TRACE_SAMPLING = 0.0005; // 0.05% sampling rate
}

extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;

PlaybackRunnerEvent::PlaybackRunnerEvent(framenum_t frameNumber, std::shared_ptr<PlaybackSession> session)
    : EventBase(frameNumber), session_(session) {}

Result<framenum_t> PlaybackRunnerEvent::executeImpl() {
    std::shared_ptr<SamplingSpan> runnerSpan = nullptr;
    if (observability) {
        auto parentSpan = session_->getSpan();
        if (parentSpan) {
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
    }

    // Check if session has been cancelled
    if (session_->isCancelled()) {
        debug("PlaybackRunnerEvent detected cancellation, performing teardown");

        // Perform teardown
        performTeardown();

        // Invoke finish callback
        session_->invokeOnFinish();

        if (runnerSpan) {
            runnerSpan->setAttribute("runner.cancelled", true);
            runnerSpan->setSuccess();
        }

        return Result<framenum_t>{this->frameNumber};
    }

    // Invoke onStart callback on first execution
    static thread_local bool hasStarted = false;
    if (!hasStarted) {
        session_->invokeOnStart();
        hasStarted = true;
    }

    // Emit DMX frames for all tracks at current frame
    auto dmxResult = emitDmxFrames();
    if (!dmxResult.isSuccess()) {
        error("Failed to emit DMX frames: {}", dmxResult.getError()->getMessage());
        if (runnerSpan) {
            runnerSpan->setError(dmxResult.getError()->getMessage());
        }
        return dmxResult;
    }

    // Dispatch audio if needed (RTP mode)
    auto audioTransport = session_->getAudioTransport();
    if (audioTransport && audioTransport->needsPerFrameDispatch()) {
        auto audioResult = audioTransport->dispatchNextChunk(this->frameNumber);
        if (!audioResult.isSuccess()) {
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

        if (runnerSpan) {
            runnerSpan->setAttribute("runner.completed_naturally", true);
            runnerSpan->setSuccess();
        }

        return Result<framenum_t>{this->frameNumber};
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
    debug("PlaybackRunnerEvent performing teardown for animation '{}'", session_->getAnimation().metadata.title);

    // NOTE: We do NOT send DMX blackout - creatures are left in their final state
    // This is intentional to avoid dangerous rapid state changes

    // Turn off status light
    auto statusLightOff =
        std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, false);
    eventLoop->scheduleEvent(statusLightOff);

    // Stop audio if playing
    auto audioTransport = session_->getAudioTransport();
    if (audioTransport) {
        audioTransport->stop();
    }

    debug("PlaybackRunnerEvent teardown complete");
}

Result<framenum_t> PlaybackRunnerEvent::emitDmxFrames() {
    trace("emitDmxFrames called for frame {}", this->frameNumber);

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
        trackState.nextDispatchFrame = this->frameNumber + (session_->getMsPerFrame() / EVENT_LOOP_PERIOD_MS);

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
    uint32_t msPerFrame = session_->getMsPerFrame();
    framenum_t framesPerAnimFrame = msPerFrame / EVENT_LOOP_PERIOD_MS;

    return this->frameNumber + framesPerAnimFrame;
}

} // namespace creatures
