#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace creatures::runtime {

struct ActivitySnapshot {
    std::string state;
    std::optional<std::string> animationId;
    std::optional<std::string> sessionId;
    std::string reason;
    std::string startedAt;
    std::string updatedAt;
};

struct CountersSnapshot {
    uint64_t sessionsStartedTotal = 0;
    uint64_t sessionsCancelledTotal = 0;
    uint64_t idleStartedTotal = 0;
    uint64_t idleStoppedTotal = 0;
    uint64_t idleTogglesTotal = 0;
    uint64_t skipsMissingCreatureTotal = 0;
    uint64_t bgmTakeoversTotal = 0;
    uint64_t audioResetsTotal = 0;
};

struct ErrorSnapshot {
    std::string message;
    std::string timestamp;
};

struct CreatureRuntimeSnapshot {
    bool idleEnabled = true;
    ActivitySnapshot activity;
    CountersSnapshot counters;
    std::optional<std::string> bgmOwner;
    std::optional<ErrorSnapshot> lastError;
};

inline nlohmann::json activitySnapshotToJson(const ActivitySnapshot &snapshot) {
    return {{"state", snapshot.state},
            {"animation_id", snapshot.animationId ? nlohmann::json(*snapshot.animationId) : nlohmann::json(nullptr)},
            {"session_id", snapshot.sessionId ? nlohmann::json(*snapshot.sessionId) : nlohmann::json(nullptr)},
            {"reason", snapshot.reason},
            {"started_at", snapshot.startedAt},
            {"updated_at", snapshot.updatedAt}};
}

inline nlohmann::json creatureRuntimeSnapshotToJson(const CreatureRuntimeSnapshot &snapshot) {
    const auto counters = nlohmann::json{{"sessions_started_total", snapshot.counters.sessionsStartedTotal},
                                         {"sessions_cancelled_total", snapshot.counters.sessionsCancelledTotal},
                                         {"idle_started_total", snapshot.counters.idleStartedTotal},
                                         {"idle_stopped_total", snapshot.counters.idleStoppedTotal},
                                         {"idle_toggles_total", snapshot.counters.idleTogglesTotal},
                                         {"skips_missing_creature_total", snapshot.counters.skipsMissingCreatureTotal},
                                         {"bgm_takeovers_total", snapshot.counters.bgmTakeoversTotal},
                                         {"audio_resets_total", snapshot.counters.audioResetsTotal}};
    const auto lastError = snapshot.lastError ? nlohmann::json{{"message", snapshot.lastError->message},
                                                               {"timestamp", snapshot.lastError->timestamp}}
                                              : nlohmann::json(nullptr);
    return {{"idle_enabled", snapshot.idleEnabled},
            {"activity", activitySnapshotToJson(snapshot.activity)},
            {"counters", counters},
            {"bgm_owner", snapshot.bgmOwner ? nlohmann::json(*snapshot.bgmOwner) : nlohmann::json(nullptr)},
            {"last_error", lastError}};
}

} // namespace creatures::runtime
