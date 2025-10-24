//
// PlaybackSession.cpp
// State container for cooperative animation playback
//

#include "PlaybackSession.h"

#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

PlaybackSession::PlaybackSession(const Animation &animation, universe_t universe, framenum_t startingFrame,
                                 std::shared_ptr<OperationSpan> parentSpan)
    : animation_(animation), universe_(universe), startingFrame_(startingFrame), audioBuffer_(nullptr),
      audioTransport_(nullptr), cancelled_(false), onStart_(nullptr), onFinish_(nullptr), sessionSpan_(nullptr) {

    // Create observability span for this session
    if (observability) {
        if (parentSpan) {
            sessionSpan_ = observability->createChildOperationSpan("PlaybackSession", parentSpan);
        } else {
            sessionSpan_ = observability->createOperationSpan("PlaybackSession");
        }

        if (sessionSpan_) {
            sessionSpan_->setAttribute("session.animation_id", animation_.id);
            sessionSpan_->setAttribute("session.animation_title", animation_.metadata.title);
            sessionSpan_->setAttribute("session.universe", static_cast<int64_t>(universe_));
            sessionSpan_->setAttribute("session.starting_frame", static_cast<int64_t>(startingFrame_));
            sessionSpan_->setAttribute("session.tracks_count", static_cast<int64_t>(animation_.tracks.size()));
            sessionSpan_->setAttribute("session.ms_per_frame",
                                       static_cast<int64_t>(animation_.metadata.milliseconds_per_frame));
            sessionSpan_->setAttribute("session.scheduler_type", "cooperative");
        }
    }

    // Decode all track frames upfront for fast playback
    trackStates_.reserve(animation_.tracks.size());
    uint64_t totalFramesDecoded = 0;

    for (const auto &track : animation_.tracks) {
        TrackState state;
        state.creatureId = track.creature_id;
        state.currentFrameIndex = 0;
        state.nextDispatchFrame = startingFrame_;

        // Decode all frames for this track
        state.decodedFrames.reserve(track.frames.size());
        for (const auto &frameData : track.frames) {
            state.decodedFrames.push_back(decodeBase64(frameData));
        }

        totalFramesDecoded += track.frames.size();
        trackStates_.push_back(std::move(state));

        trace("Decoded {} frames for creature {} in animation '{}'", track.frames.size(), track.creature_id,
              animation_.metadata.title);
    }

    if (sessionSpan_) {
        sessionSpan_->setAttribute("session.total_frames_decoded", static_cast<int64_t>(totalFramesDecoded));
    }

    debug("Created PlaybackSession for animation '{}' on universe {} starting at frame {} ({} tracks, {} total frames)",
          animation_.metadata.title, universe_, startingFrame_, trackStates_.size(), totalFramesDecoded);
}

PlaybackSession::~PlaybackSession() {
    // Mark observability span as complete
    if (sessionSpan_) {
        if (cancelled_.load()) {
            sessionSpan_->setAttribute("session.completion_reason", "cancelled");
        } else {
            sessionSpan_->setAttribute("session.completion_reason", "natural");
        }
        sessionSpan_->setSuccess();
    }

    debug("Destroyed PlaybackSession for animation '{}' on universe {}", animation_.metadata.title, universe_);
}

void PlaybackSession::cancel() {
    bool wasAlreadyCancelled = cancelled_.exchange(true);

    if (!wasAlreadyCancelled) {
        info("Cancelled PlaybackSession for animation '{}' on universe {}", animation_.metadata.title, universe_);

        if (sessionSpan_) {
            sessionSpan_->setAttribute("session.cancelled", true);
        }
    } else {
        debug("PlaybackSession for animation '{}' was already cancelled", animation_.metadata.title);
    }
}

void PlaybackSession::setStartingFrame(framenum_t frame) {
    startingFrame_ = frame;

    // Update all track states to dispatch at the new starting frame
    for (auto &trackState : trackStates_) {
        trackState.nextDispatchFrame = frame;
    }

    debug("Updated starting frame to {} for animation '{}' on universe {}", frame, animation_.metadata.title,
          universe_);
}

} // namespace creatures
