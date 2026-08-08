
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

// Stage caps. Enforced by the DB-layer parser; the controller exposes JSON
// shape errors as 400s and the parser does the same for cap violations.
inline constexpr std::size_t MAX_STAGE_TITLE = 256;
inline constexpr std::size_t MAX_STAGE_NOTES = 16384;

// One placement per creature, and a creature only exists on the stage if it
// has an audio lane — so the ceiling is the 16 creature lanes (lane 17 is BGM).
inline constexpr std::size_t MAX_STAGE_PLACEMENTS = 16;

// The stage is a 10 m cube centred on the listener. Anything outside this is
// far more likely to be a unit mix-up (centimetres, or the console's old
// absolute frame) than a real placement, so reject it loudly.
inline constexpr float STAGE_COORD_LIMIT = 5.0f;

// Bumped when the coordinate frame or field meanings change in a way clients
// must react to. v1 = listener-at-origin (see the header comment on Stage).
inline constexpr int32_t STAGE_CURRENT_VERSION = 1;

/// The geometry the server actually uses out of a placement. Extracted from
/// the stored JSON by `stagePlacements()`; the parser has already validated
/// every field, so extraction can't fail.
///
/// COORDINATE FRAME (issue #119) — this is the load-bearing convention:
///
///   The origin (0,0,0) IS the listener's head, facing -Z. Metres, Y-up,
///   right-handed — the same axes AVAudioEngine uses in the console's spatial
///   renderer, but re-origined so the stage spans symmetrically around the
///   listener rather than being crammed into negative Z.
///
///     +x  to the listener's right      -x  to the listener's left
///     +y  above the listener's ears    -y  below
///     -z  in front of the listener     +z  behind them
///
///   `yaw` is which way the creature faces, in degrees, same convention as a
///   bearing: 0 = facing +Z, +90 = facing +X. It is NOT relative to the
///   listener; it's an absolute heading in the stage frame.
struct StagePlacement {
    std::string creature_id;
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float yaw{0.0f}; // degrees, normalized to (-180, 180]

    bool operator==(const StagePlacement &) const = default;
};

/// A saved stage: where each creature sits and which way it faces.
///
/// Two blobs are carried as opaque `nlohmann::json` rather than typed fields,
/// for the same reason Storyboard carries `tiles` that way — the console owns
/// settings the server has no opinion about, and routing them through an
/// oatpp DTO would silently strip any key this struct doesn't know:
///
///   * `placements` — the server reads creature_id/x/y/z/yaw and validates
///     them, but preserves per-placement extras (gain, muted, audio_channel,
///     display colour, …) verbatim.
///   * `audio` — entirely console-owned mix settings (monitoring delay,
///     playout delay, background-music gain, reverb blend). The server never
///     interprets these; it stores and serves them so one document drives both
///     spatial audio and head-look instead of two drifting copies.
struct Stage {
    std::string id;
    std::string title;
    std::string notes; // free-form, may be empty
    int32_t version{STAGE_CURRENT_VERSION};
    // Always an array. Shallow-validated per placement; extras preserved.
    nlohmann::json placements = nlohmann::json::array();
    // Always an object. Never interpreted server-side.
    nlohmann::json audio = nlohmann::json::object();
    // Wall-clock milliseconds since epoch — server-managed, not honored from
    // the client. created_at is set on first insert and never changes.
    int64_t created_at{0};
    int64_t updated_at{0};
};

/// Extract the typed geometry from a parsed Stage. Infallible: the parser has
/// already checked that every placement carries a creature_id and four finite,
/// in-range numbers.
[[nodiscard]] std::vector<StagePlacement> stagePlacements(const Stage &stage);

/// Normalize an angle in degrees to (-180, 180]. Shared by the parser and the
/// gaze layer so "which way is shorter round the circle" is answered the same
/// way in both.
[[nodiscard]] float normalizeDegrees(float degrees);

#include OATPP_CODEGEN_BEGIN(DTO)

// Swagger-only DTO. The controller does NOT serialize through this — it
// returns raw JSON via ResponseFactory::createResponse to preserve the opaque
// placement extras and the console-owned `audio` block, exactly as
// StoryboardController does for `tiles`.
class StageDto : public oatpp::DTO {

    DTO_INIT(StageDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Stage UUID. Server-generated on create."; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(title) { info->description = "Human-readable stage title, e.g. 'Mainstage' or 'Travel'."; }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(notes) {
        info->description = "Free-form notes attached to the stage.";
        info->required = false;
    }
    DTO_FIELD(String, notes);

    DTO_FIELD_INFO(version) {
        info->description = "Coordinate-frame version. 1 = listener at the origin facing -Z, metres, Y-up.";
    }
    DTO_FIELD(Int32, version);

    DTO_FIELD_INFO(placements) {
        info->description = "Array of creature placements. Each has creature_id plus x/y/z in metres relative to the "
                            "listener (who is at the origin, facing -Z) and yaw in degrees (0 = facing +Z, +90 = "
                            "facing +X). Coordinates are clamped to +/-5 m. Extra per-placement keys are preserved "
                            "verbatim.";
    }
    DTO_FIELD(Any, placements);

    DTO_FIELD_INFO(audio) {
        info->description = "Console-owned spatial-audio mix settings. Stored and returned verbatim; the server "
                            "never interprets this object.";
        info->required = false;
    }
    DTO_FIELD(Any, audio);

    DTO_FIELD_INFO(created_at) {
        info->description = "Wall-clock milliseconds since Unix epoch when the stage was first persisted. "
                            "Server-managed; ignored on PUT.";
    }
    DTO_FIELD(Int64, created_at);

    DTO_FIELD_INFO(updated_at) {
        info->description = "Wall-clock milliseconds since Unix epoch of the most recent edit. Server-managed; "
                            "ignored on PUT. Animations compare their source_stage_updated_at against this to "
                            "decide whether they are stale.";
    }
    DTO_FIELD(Int64, updated_at);
};

#include OATPP_CODEGEN_END(DTO)

/// Serialize a Stage back to its canonical JSON shape — the same shape the
/// client sent on POST/PUT and the same shape returned from GET.
[[nodiscard]] nlohmann::json stageToJson(const Stage &stage);

} // namespace creatures
