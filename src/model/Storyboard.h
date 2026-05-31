
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

// Storyboard caps. Mirror the contract pinned in
// creature-console/docs/storyboard-server-contract.md. Enforced by the DB-layer
// parser; the controller exposes JSON shape errors as 400s and the parser does
// the same for cap violations. Bumping these is a contract change — coordinate
// with the client side first.
inline constexpr std::size_t MAX_STORYBOARD_TITLE = 256;
inline constexpr std::size_t MAX_STORYBOARD_NOTES = 16384;
inline constexpr std::size_t MAX_STORYBOARD_TILES = 200;
inline constexpr std::size_t MAX_STORYBOARD_TILE_LABEL = 256;

// A saved storyboard. Visual tiles a user can tap from the Console; the tile
// `action` payload is opaque — the server stores it verbatim and the client
// owns the action type vocabulary. Keeping `tiles` as a free-form nlohmann
// blob is the load-bearing forward-compat seam: when the client invents a new
// action type, the server stores + serves it back without code changes.
struct Storyboard {
    std::string id;
    std::string title;
    std::string notes; // free-form, may be empty
    // Always an array. Per-tile shallow validation (id, label length) happens
    // in the parser; everything inside `action` is preserved verbatim.
    nlohmann::json tiles = nlohmann::json::array();
    // Wall-clock milliseconds since epoch — server-managed, not honored from
    // the client. created_at is set on first insert and never changes.
    int64_t created_at{0};
    int64_t updated_at{0};
};

#include OATPP_CODEGEN_BEGIN(DTO)

// Swagger-only DTO. The controller does NOT serialize through this — it
// returns raw JSON via ResponseFactory::createResponse to preserve opaque
// `action` keys. Going through oatpp's strict serializer would strip any tile
// field the DTO doesn't know about, which is exactly what we're trying to
// avoid. This DTO exists so the OpenAPI spec can name the response type;
// `tiles` is left as `Any` in the spec since its action shape is open.
class StoryboardDto : public oatpp::DTO {

    DTO_INIT(StoryboardDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Storyboard UUID. Server-generated on create."; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(title) { info->description = "Human-readable storyboard title."; }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(notes) {
        info->description = "Free-form notes attached to the storyboard.";
        info->required = false;
    }
    DTO_FIELD(String, notes);

    DTO_FIELD_INFO(tiles) {
        info->description = "Array of storyboard tiles. Each tile has an opaque `action` object — see "
                            "creature-console/docs/storyboard-server-contract.md for the client-defined "
                            "action types and shapes. Server stores + returns the action verbatim.";
    }
    DTO_FIELD(Any, tiles);

    DTO_FIELD_INFO(created_at) {
        info->description = "Wall-clock milliseconds since Unix epoch when the storyboard was first persisted. "
                            "Server-managed; ignored on PUT.";
    }
    DTO_FIELD(Int64, created_at);

    DTO_FIELD_INFO(updated_at) {
        info->description = "Wall-clock milliseconds since Unix epoch of the most recent edit. Server-managed; "
                            "ignored on PUT.";
    }
    DTO_FIELD(Int64, updated_at);
};

#include OATPP_CODEGEN_END(DTO)

// Serialize a Storyboard back to its canonical JSON shape — the same shape the
// client sent on POST/PUT and the same shape returned from GET. Tiles are
// dumped as-is (including any unknown action keys).
nlohmann::json storyboardToJson(const Storyboard &storyboard);

} // namespace creatures
