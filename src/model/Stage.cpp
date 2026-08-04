
#include <cmath>

#include <nlohmann/json.hpp>

#include "Stage.h"

namespace creatures {

float normalizeDegrees(float degrees) {
    if (!std::isfinite(degrees)) {
        return 0.0f;
    }
    // Land in [0, 360) first, then fold the top half negative so the result is
    // (-180, 180]. Doing it this way (rather than a while-loop) keeps the cost
    // constant even if a caller hands us something absurd like 1e6 degrees.
    float wrapped = std::fmod(degrees, 360.0f);
    if (wrapped < 0.0f) {
        wrapped += 360.0f;
    }
    if (wrapped > 180.0f) {
        wrapped -= 360.0f;
    }
    // fmod of a value just under 360 can round to exactly -180; normalize the
    // sign so -180 and +180 never both appear for the same physical heading.
    if (wrapped == -180.0f) {
        wrapped = 180.0f;
    }
    return wrapped;
}

std::vector<StagePlacement> stagePlacements(const Stage &stage) {
    std::vector<StagePlacement> out;
    if (!stage.placements.is_array()) {
        return out;
    }
    out.reserve(stage.placements.size());
    for (const auto &entry : stage.placements) {
        // The parser guarantees these are present and well-formed; the guards
        // here just make this safe to call on a hand-constructed Stage.
        if (!entry.is_object() || !entry.contains("creature_id") || !entry["creature_id"].is_string()) {
            continue;
        }
        StagePlacement placement;
        placement.creature_id = entry["creature_id"].get<std::string>();
        placement.x = entry.value("x", 0.0f);
        placement.y = entry.value("y", 0.0f);
        placement.z = entry.value("z", 0.0f);
        placement.yaw = normalizeDegrees(entry.value("yaw", 0.0f));
        out.push_back(std::move(placement));
    }
    return out;
}

nlohmann::json stageToJson(const Stage &stage) {
    nlohmann::json j;
    j["id"] = stage.id;
    j["title"] = stage.title;
    j["notes"] = stage.notes;
    j["version"] = stage.version;
    // placements and audio are already nlohmann::json carrying console-owned
    // keys. Don't iterate + reconstruct — that would drop anything this struct
    // doesn't model, which is exactly what we're preserving.
    j["placements"] = stage.placements;
    j["audio"] = stage.audio;
    j["created_at"] = stage.created_at;
    j["updated_at"] = stage.updated_at;
    return j;
}

} // namespace creatures
