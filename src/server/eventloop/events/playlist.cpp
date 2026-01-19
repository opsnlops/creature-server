
#include <algorithm>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "spdlog/spdlog.h"

#include "model/PlaylistStatus.h"
#include "server/animation/SessionManager.h"
#include "server/animation/player.h"
#include "server/database.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"

#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/cache.h"
#include "util/websocketUtils.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;

PlaylistEvent::PlaylistEvent(framenum_t frameNumber_, universe_t universe_)
    : EventBase(frameNumber_), activeUniverse(universe_) {}

Result<framenum_t> PlaylistEvent::executeImpl() {
    auto span = observability ? observability->createOperationSpan("playlist_event.execute") : nullptr;
    if (span) {
        span->setAttribute("active_universe", activeUniverse);
    }

    debug("hello from a playlist event for universe {}", activeUniverse);

    if (auto dependencyResult = ensureDependencies(span); !dependencyResult.isSuccess()) {
        return Result<framenum_t>{dependencyResult.getError().value()};
    }

    metrics->incrementPlaylistsEventsProcessed();

    auto playlistState = sessionManager->getPlaylistState(activeUniverse);
    if (span) {
        span->setAttribute("playlist_state", static_cast<int>(playlistState));
    }

    if (auto stateResult = handlePlaylistState(playlistState, span)) {
        return *stateResult;
    }

    if (shouldSkipForActiveSession(span)) {
        return Result<framenum_t>{this->frameNumber};
    }

    PlaylistStatus activePlaylistStatus{};
    if (auto statusResult = loadActivePlaylistStatus(activePlaylistStatus, span)) {
        return *statusResult;
    }

    debug("the active playlistStatus snapshot is {}", activePlaylistStatus.playlist);

    auto playlistResult = fetchPlaylist(activePlaylistStatus, span);
    if (!playlistResult.isSuccess()) {
        return Result<framenum_t>{playlistResult.getError().value()};
    }
    auto playlist = playlistResult.getValue().value();
    debug("playlist found. name: {}", playlist.name);

    auto chosenResult = chooseWeightedAnimation(playlist);
    if (!chosenResult.isSuccess()) {
        return Result<framenum_t>{chosenResult.getError().value()};
    }
    auto chosenAnimation = chosenResult.getValue().value();
    debug("...and the chosen one is {}", chosenAnimation);
    if (span) {
        span->setAttribute("chosen_animation", chosenAnimation);
    }

    auto animationResult = fetchAnimation(chosenAnimation, span);
    if (!animationResult.isSuccess()) {
        return Result<framenum_t>{animationResult.getError().value()};
    }
    auto animation = animationResult.getValue().value();

    auto involvedCreatures = collectInvolvedCreatures(animation);
    if (span) {
        span->setAttribute("playlist.animation_creature_count", static_cast<int64_t>(involvedCreatures.size()));
    }

    auto scheduleResult = scheduleChosenAnimation(animation);
    if (!scheduleResult.isSuccess()) {
        return Result<framenum_t>{scheduleResult.getError().value()};
    }
    auto lastFrame = scheduleResult.getValue().value();

    debug("scheduled animation {} on universe {}. Last frame: {}", animation.metadata.title, activeUniverse, lastFrame);

    scheduleNextPlaylistEvent(lastFrame);

    updatePlaylistStatus(activePlaylistStatus, chosenAnimation);

    startIdleLoopsForUniverse(involvedCreatures, span);

    debug("scheduled next event for frame {}. later!", lastFrame + 1);
    return Result<framenum_t>{lastFrame};
}

Result<void> PlaylistEvent::ensureDependencies(std::shared_ptr<OperationSpan> span) {
    if (!metrics) {
        const std::string errorMessage = "PlaylistEvent: metrics unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
    if (!sessionManager) {
        const std::string errorMessage = "PlaylistEvent: session manager unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
    if (!db) {
        const std::string errorMessage = "PlaylistEvent: database unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
    if (!eventLoop) {
        const std::string errorMessage = "PlaylistEvent: event loop unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }

    return Result<void>{};
}

std::optional<Result<framenum_t>> PlaylistEvent::handlePlaylistState(PlaylistState playlistState,
                                                                     std::shared_ptr<OperationSpan> span) {
    switch (playlistState) {
    case PlaylistState::None:
    case PlaylistState::Stopped: {
        std::string errorMessage =
            fmt::format("Playlist on universe {} is stopped or doesn't exist. Cleaning up.", activeUniverse);
        info(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        if (span) {
            span->setAttribute("reason", "stopped_or_none");
            span->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }
    case PlaylistState::Interrupted: {
        info("Playlist on universe {} is interrupted, skipping scheduled event", activeUniverse);
        if (span) {
            span->setAttribute("reason", "interrupted");
            span->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }
    case PlaylistState::Active:
        debug("Playlist on universe {} is active, continuing", activeUniverse);
        return std::nullopt;
    }

    return std::nullopt;
}

bool PlaylistEvent::shouldSkipForActiveSession(std::shared_ptr<OperationSpan> span) {
    if (!sessionManager->hasActiveNonIdleSession(activeUniverse)) {
        return false;
    }

    info("Playlist on universe {} has active non-idle session, skipping scheduled event", activeUniverse);
    if (span) {
        span->setAttribute("reason", "non_idle_session_present");
        span->setSuccess();
    }
    return true;
}

std::optional<Result<framenum_t>> PlaylistEvent::loadActivePlaylistStatus(PlaylistStatus &playlistStatus,
                                                                          std::shared_ptr<OperationSpan> span) {
    auto activePlaylistStatusOpt = sessionManager->getPlaylistStatus(activeUniverse);
    if (!activePlaylistStatusOpt || activePlaylistStatusOpt->playlist.empty()) {
        std::string errorMessage = fmt::format(
            "Playlist state is Active but no playlist snapshot exists for universe {}. Cleaning up.", activeUniverse);
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        if (span) {
            span->setAttribute("reason", "cache_missing");
            span->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }

    playlistStatus = *activePlaylistStatusOpt;
    return std::nullopt;
}

Result<Playlist> PlaylistEvent::fetchPlaylist(const PlaylistStatus &playlistStatus,
                                              std::shared_ptr<OperationSpan> span) {
    auto dbSpan = observability ? observability->createChildOperationSpan("music_event.db_lookup", span) : nullptr;
    if (dbSpan) {
        dbSpan->setAttribute("playlist.id", playlistStatus.playlist);
    }

    auto playListResult = db->getPlaylist(playlistStatus.playlist, dbSpan);
    if (!playListResult.isSuccess()) {
        std::string errorMessage = fmt::format("Playlist ID {} not found while in a playlist event. halting playback.",
                                               playlistStatus.playlist);
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        if (dbSpan) {
            dbSpan->setError(errorMessage);
        }
        return Result<Playlist>{ServerError(ServerError::InternalError, errorMessage)};
    }

    auto playlist = playListResult.getValue().value();
    if (dbSpan) {
        dbSpan->setSuccess();
    }

    return Result<Playlist>{playlist};
}

Result<std::string> PlaylistEvent::chooseWeightedAnimation(const Playlist &playlist) {
    std::vector<std::string> choices;
    for (const auto &playlistItem : playlist.items) {
        for (uint32_t i = 0; i < playlistItem.weight; i++) {
            choices.push_back(playlistItem.animation_id);
        }
        debug("added an animation to the list. {} now possible", choices.size());
    }

    if (choices.empty()) {
        std::string errorMessage =
            fmt::format("Playlist {} has no animations to schedule. Halting playlist playback.", playlist.id);
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<std::string>{ServerError(ServerError::InternalError, errorMessage)};
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, choices.size() - 1);
    size_t theChosenOne = dis(gen);

    return Result<std::string>{choices[theChosenOne]};
}

Result<Animation> PlaylistEvent::fetchAnimation(const std::string &animationId, std::shared_ptr<OperationSpan> span) {
    auto animationSpan =
        observability ? observability->createChildOperationSpan("music_event.animation_lookup", span) : nullptr;
    if (animationSpan) {
        animationSpan->setAttribute("animation.id", animationId);
    }

    Result<Animation> animationResult = db->getAnimation(animationId, animationSpan);
    if (!animationResult.isSuccess()) {
        std::string errorMessage =
            fmt::format("Animation ID {} not found while in a playlist event. Halting playlist playback.", animationId);
        warn(errorMessage);
        if (animationSpan) {
            animationSpan->setError(errorMessage);
        }
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<Animation>{ServerError(ServerError::InternalError, errorMessage)};
    }

    auto animation = animationResult.getValue().value();
    if (animationSpan) {
        animationSpan->setAttribute("animation.title", animation.metadata.title);
        animationSpan->setSuccess();
    }

    return Result<Animation>{animation};
}

std::unordered_set<creatureId_t> PlaylistEvent::collectInvolvedCreatures(const Animation &animation) {
    std::unordered_set<creatureId_t> involvedCreatures;
    involvedCreatures.reserve(animation.tracks.size());
    for (const auto &track : animation.tracks) {
        if (!track.creature_id.empty()) {
            involvedCreatures.insert(track.creature_id);
        }
    }

    return involvedCreatures;
}

Result<framenum_t> PlaylistEvent::scheduleChosenAnimation(const Animation &animation) {
    auto scheduleResult = scheduleAnimation(eventLoop->getNextFrameNumber(), animation, activeUniverse,
                                            creatures::runtime::ActivityReason::Playlist);
    if (!scheduleResult.isSuccess()) {
        std::string errorMessage = fmt::format("Unable to schedule animation: {}. Halting playlist playback.",
                                               scheduleResult.getError().value().getMessage());
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    return Result<framenum_t>{scheduleResult.getValue().value()};
}

void PlaylistEvent::scheduleNextPlaylistEvent(framenum_t lastFrame) {
    auto nextEvent = std::make_shared<PlaylistEvent>(lastFrame + 1, activeUniverse);
    eventLoop->scheduleEvent(nextEvent);
}

void PlaylistEvent::updatePlaylistStatus(PlaylistStatus playlistStatus, const std::string &chosenAnimation) {
    playlistStatus.current_animation = chosenAnimation;
    sessionManager->setPlaylistStatus(activeUniverse, playlistStatus);
    sendPlaylistUpdate(playlistStatus);
}

void PlaylistEvent::startIdleLoopsForUniverse(const std::unordered_set<creatureId_t> &involvedCreatures,
                                              std::shared_ptr<OperationSpan> span) {
    if (!creatureUniverseMap) {
        return;
    }

    auto allCreatures = creatureUniverseMap->getAllKeys();
    size_t idleCandidates = 0;
    for (const auto &creatureId : allCreatures) {
        if (involvedCreatures.contains(creatureId)) {
            continue;
        }
        try {
            if (auto universePtr = creatureUniverseMap->get(creatureId);
                !universePtr || *universePtr != activeUniverse) {
                continue;
            }
        } catch (const std::out_of_range &) {
            continue;
        }
        idleCandidates++;
        creatures::ws::CreatureService::startIdleIfNeeded(creatureId, span);
    }
    if (span) {
        span->setAttribute("playlist.idle_candidates", static_cast<int64_t>(idleCandidates));
    }
}

/**
 * Send a playlist update to all clients
 *
 * This can be used to let the clients know that the playlist has changed, and what the current animation is.
 *
 * @param playlistStatus the PlaylistStatus to send
 */
void PlaylistEvent::sendPlaylistUpdate(const PlaylistStatus &playlistStatus) {

    if (const auto result = broadcastPlaylistStatusToAllClients(playlistStatus); !result.isSuccess()) {
        warn("Unable to broadcast playlist status to all clients: {}", result.getError().value().getMessage());
        // Just log. It's not a critical error.
    }
}

void PlaylistEvent::sendEmptyPlaylistUpdate(universe_t universe) {
    PlaylistStatus emptyStatus{};
    emptyStatus.universe = universe;
    emptyStatus.playlist = "";
    emptyStatus.playing = false;
    emptyStatus.current_animation = "";
    sendPlaylistUpdate(emptyStatus);
}

} // namespace creatures
