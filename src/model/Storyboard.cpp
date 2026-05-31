
#include <nlohmann/json.hpp>

#include "Storyboard.h"

namespace creatures {

nlohmann::json storyboardToJson(const Storyboard &storyboard) {
    nlohmann::json j;
    j["id"] = storyboard.id;
    j["title"] = storyboard.title;
    j["notes"] = storyboard.notes;
    // tiles is already a nlohmann::json array carrying opaque action payloads.
    // Don't iterate + reconstruct — that'd lose any keys the C++ struct
    // doesn't know about, which defeats the forward-compat seam.
    j["tiles"] = storyboard.tiles;
    j["created_at"] = storyboard.created_at;
    j["updated_at"] = storyboard.updated_at;
    return j;
}

} // namespace creatures
