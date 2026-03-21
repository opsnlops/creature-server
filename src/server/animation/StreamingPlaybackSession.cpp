#include "StreamingPlaybackSession.h"

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

StreamingPlaybackSession::StreamingPlaybackSession(const Animation &animation, universe_t universe,
                                                    framenum_t startingFrame,
                                                    std::shared_ptr<OperationSpan> parentSpan)
    : animation_(animation), universe_(universe), startingFrame_(startingFrame) {

    if (parentSpan && observability) {
        sessionSpan_ = observability->createChildOperationSpan("StreamingPlaybackSession", parentSpan);
        if (sessionSpan_) {
            sessionSpan_->setAttribute("session.id", sessionId_);
            sessionSpan_->setAttribute("session.universe", static_cast<int64_t>(universe));
            sessionSpan_->setAttribute("animation.id", animation.id);
            sessionSpan_->setAttribute("animation.title", animation.metadata.title);
        }
    }

    info("StreamingPlaybackSession created: session={}, animation='{}', universe={}", sessionId_,
         animation.metadata.title, universe);
}

StreamingPlaybackSession::~StreamingPlaybackSession() {
    debug("StreamingPlaybackSession destroyed: session={}", sessionId_);
    if (sessionSpan_) {
        sessionSpan_->setSuccess();
    }
}

void StreamingPlaybackSession::cancel() {
    cancelled_.store(true);
    debug("StreamingPlaybackSession cancelled: session={}", sessionId_);
}

StreamingPlaybackSession::StreamingTrackState &
StreamingPlaybackSession::findOrCreateTrack(const creatureId_t &creatureId) {
    for (auto &ts : trackStates_) {
        if (ts.creatureId == creatureId) {
            return ts;
        }
    }
    trackStates_.push_back({creatureId, {}, 0});
    return trackStates_.back();
}

const StreamingPlaybackSession::StreamingTrackState *
StreamingPlaybackSession::findTrack(const creatureId_t &creatureId) const {
    for (const auto &ts : trackStates_) {
        if (ts.creatureId == creatureId) {
            return &ts;
        }
    }
    return nullptr;
}

void StreamingPlaybackSession::appendFrames(const creatureId_t &creatureId,
                                             const std::vector<std::vector<uint8_t>> &newFrames) {
    std::lock_guard<std::mutex> lock(trackMutex_);
    auto &track = findOrCreateTrack(creatureId);
    track.frames.insert(track.frames.end(), newFrames.begin(), newFrames.end());

    trace("Appended {} frames for creature {} (total: {})", newFrames.size(), creatureId, track.frames.size());
}

void StreamingPlaybackSession::markComplete() {
    complete_.store(true);
    debug("StreamingPlaybackSession stream complete: session={}", sessionId_);
}

bool StreamingPlaybackSession::isWaitingForData(const creatureId_t &creatureId) const {
    if (complete_.load()) {
        return false; // Stream is done, no more data coming
    }

    std::lock_guard<std::mutex> lock(trackMutex_);
    const auto *track = findTrack(creatureId);
    if (!track) {
        return true; // No track yet, still waiting
    }

    return track->playbackIndex >= track->frames.size();
}

uint32_t StreamingPlaybackSession::availableFrames(const creatureId_t &creatureId) const {
    std::lock_guard<std::mutex> lock(trackMutex_);
    const auto *track = findTrack(creatureId);
    if (!track) {
        return 0;
    }
    return static_cast<uint32_t>(track->frames.size());
}

uint32_t StreamingPlaybackSession::currentFrameIndex(const creatureId_t &creatureId) const {
    std::lock_guard<std::mutex> lock(trackMutex_);
    const auto *track = findTrack(creatureId);
    if (!track) {
        return 0;
    }
    return track->playbackIndex;
}

std::vector<uint8_t> StreamingPlaybackSession::getFrame(const creatureId_t &creatureId, uint32_t frameIndex) const {
    std::lock_guard<std::mutex> lock(trackMutex_);
    const auto *track = findTrack(creatureId);
    if (!track || frameIndex >= track->frames.size()) {
        return {};
    }
    return track->frames[frameIndex];
}

void StreamingPlaybackSession::advanceFrame(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(trackMutex_);
    auto &track = findOrCreateTrack(creatureId);
    if (track.playbackIndex < track.frames.size()) {
        track.playbackIndex++;
    }
}

bool StreamingPlaybackSession::isAllTracksFinished() const {
    if (!complete_.load()) {
        return false; // More data might be coming
    }

    std::lock_guard<std::mutex> lock(trackMutex_);
    if (trackStates_.empty()) {
        return true;
    }

    for (const auto &track : trackStates_) {
        if (track.playbackIndex < track.frames.size()) {
            return false;
        }
    }
    return true;
}

} // namespace creatures
