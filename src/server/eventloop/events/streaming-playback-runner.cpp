//
// streaming-playback-runner.cpp
// Cooperative streaming playback runner for incremental animation delivery
//

#include "types.h"

#include <algorithm>
#include <memory>

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include "server/animation/StreamingPlaybackSession.h"
#include "server/audio/AudioTransport.h"
#include "server/config.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {

namespace {
constexpr double STREAMING_RUNNER_TRACE_SAMPLING = 0.0005;

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

StreamingPlaybackRunnerEvent::StreamingPlaybackRunnerEvent(framenum_t frameNumber,
                                                            std::shared_ptr<StreamingPlaybackSession> session)
    : EventBase(frameNumber), session_(std::move(session)) {}

Result<framenum_t> StreamingPlaybackRunnerEvent::executeImpl() {
    if (!session_) {
        std::string errorMsg = "StreamingPlaybackRunnerEvent has no session";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    if (!eventLoop) {
        std::string errorMsg = "StreamingPlaybackRunnerEvent missing event loop";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    std::shared_ptr<SamplingSpan> runnerSpan = nullptr;
    if (observability) {
        if (auto parentSpan = session_->getSpan()) {
            runnerSpan = observability->createSamplingSpan("streaming_playback_runner.execute", parentSpan,
                                                           STREAMING_RUNNER_TRACE_SAMPLING);
        } else {
            runnerSpan = observability->createSamplingSpan("streaming_playback_runner.execute",
                                                           STREAMING_RUNNER_TRACE_SAMPLING);
        }
    }

    // Check cancellation
    if (session_->isCancelled()) {
        debug("StreamingPlaybackRunnerEvent detected cancellation, performing teardown");
        performTeardown();
        session_->invokeOnFinish();

        if (runnerSpan) {
            runnerSpan->setAttribute("runner.cancelled", true);
            runnerSpan->setSuccess();
        }
        return Result{this->frameNumber};
    }

    // Invoke onStart on first execution
    session_->invokeOnStart();

    // Emit DMX frames for all tracks at current position
    auto dmxResult = emitDmxFrames();
    if (!dmxResult.isSuccess()) {
        error("StreamingPlaybackRunnerEvent: DMX emission failed: {}", dmxResult.getError()->getMessage());
        if (runnerSpan) {
            runnerSpan->setError(dmxResult.getError()->getMessage());
        }
        return dmxResult;
    }

    // Dispatch audio if needed
    if (auto audioTransport = session_->getAudioTransport();
        audioTransport && audioTransport->needsPerFrameDispatch()) {
        if (auto audioResult = audioTransport->dispatchNextChunk(this->frameNumber); !audioResult.isSuccess()) {
            warn("Streaming audio dispatch failed: {}", audioResult.getError()->getMessage());
        }
    }

    // Check if all tracks are finished (stream complete + all frames consumed)
    if (session_->isAllTracksFinished()) {
        debug("StreamingPlaybackRunnerEvent: all tracks finished, completing playback");
        performTeardown();
        session_->invokeOnFinish();

        if (runnerSpan) {
            runnerSpan->setAttribute("runner.completed_naturally", true);
            runnerSpan->setSuccess();
        }
        return Result{this->frameNumber};
    }

    // Schedule next runner event
    framenum_t nextFrame = calculateNextFrameNumber();
    auto nextRunner = std::make_shared<StreamingPlaybackRunnerEvent>(nextFrame, session_);
    eventLoop->scheduleEvent(nextRunner);

    if (runnerSpan) {
        runnerSpan->setAttribute("runner.next_frame", static_cast<int64_t>(nextFrame));
        runnerSpan->setSuccess();
    }

    return Result<framenum_t>{this->frameNumber};
}

Result<framenum_t> StreamingPlaybackRunnerEvent::emitDmxFrames() {
    if (!creatureCache) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "Creature cache unavailable")};
    }
    if (!eventLoop) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "Event loop unavailable")};
    }

    // Get the animation's tracks to know which creatures are involved
    const auto &animation = session_->getAnimation();
    for (const auto &track : animation.tracks) {
        auto creatureId = track.creature_id;
        if (creatureId.empty()) {
            continue;
        }

        uint32_t frameIdx = session_->currentFrameIndex(creatureId);

        // Check for buffer underrun (waiting for more data from stream)
        if (session_->isWaitingForData(creatureId)) {
            // Hold last known frame - don't jerk to rest position
            trace("StreamingPlaybackRunnerEvent: waiting for data on creature {}", creatureId);
            continue;
        }

        auto frameData = session_->getFrame(creatureId, frameIdx);
        if (frameData.empty()) {
            continue; // No data available
        }

        // Look up creature for channel offset
        std::shared_ptr<Creature> creature;
        if (creatureCache->contains(creatureId)) {
            creature = creatureCache->get(creatureId);
        } else {
            auto creatureResult = db->getCreature(creatureId, nullptr);
            if (!creatureResult.isSuccess()) {
                error("Creature {} not found during streaming playback", creatureId);
                continue;
            }
            creature = std::make_shared<Creature>(creatureResult.getValue().value());
            creatureCache->put(creatureId, creature);
        }

        // Create and schedule DMX event
        auto dmxEvent = std::make_shared<DMXEvent>(this->frameNumber);
        dmxEvent->universe = session_->getUniverse();
        dmxEvent->channelOffset = creature->channel_offset;
        dmxEvent->data.assign(frameData.begin(), frameData.end());
        eventLoop->scheduleEvent(dmxEvent);

        // Advance playback position
        session_->advanceFrame(creatureId);
    }

    return Result<framenum_t>{this->frameNumber};
}

void StreamingPlaybackRunnerEvent::performTeardown() {
    if (!session_) {
        return;
    }

    debug("StreamingPlaybackRunnerEvent performing teardown for '{}'", session_->getAnimation().metadata.title);

    if (eventLoop) {
        auto statusLightOff =
            std::make_shared<StatusLightEvent>(eventLoop->getNextFrameNumber(), StatusLight::Animation, false);
        eventLoop->scheduleEvent(statusLightOff);
    }

    if (const auto audioTransport = session_->getAudioTransport()) {
        audioTransport->stop();
    }

    debug("StreamingPlaybackRunnerEvent teardown complete");
}

framenum_t StreamingPlaybackRunnerEvent::calculateNextFrameNumber() const {
    const uint32_t msPerFrame = session_->getMsPerFrame();
    return this->frameNumber + frameStepForMs(msPerFrame);
}

} // namespace creatures
