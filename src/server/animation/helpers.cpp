
#include "spdlog/spdlog.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "model/DialogScript.h"
#include "server/database.h"

#include "util/Result.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

namespace creatures {

Result<creatures::Track> Database::parseTrackJson(json trackJson) { return trackFromJson(std::move(trackJson)); }

Result<creatures::Track> Database::trackFromJson(json trackJson) {

    debug("attempting to create a Track from JSON via trackFromJson()");

    try {

        auto track = Track();
        track.id = trackJson["id"];
        debug("id: {}", track.id);

        if (track.id.empty()) {
            std::string errorMessage = "Track id is empty";
            warn(errorMessage);
            return ServerError(ServerError::InvalidData, errorMessage);
        }

        track.animation_id = trackJson["animation_id"];
        debug("animation_id: {}", track.animation_id);

        if (track.animation_id.empty()) {
            std::string errorMessage = "Track animation_id is empty";
            warn(errorMessage);
            return ServerError(ServerError::InvalidData, errorMessage);
        }

        track.creature_id = trackJson.value("creature_id", "");
        track.fixture_id = trackJson.value("fixture_id", "");
        debug("creature_id: '{}', fixture_id: '{}'", track.creature_id, track.fixture_id);

        const bool hasCreature = !track.creature_id.empty();
        const bool hasFixture = !track.fixture_id.empty();
        if (hasCreature == hasFixture) {
            std::string errorMessage = hasCreature
                                           ? "Track must have exactly one of creature_id or fixture_id, not both"
                                           : "Track must have exactly one of creature_id or fixture_id, got neither";
            warn(errorMessage);
            return ServerError(ServerError::InvalidData, errorMessage);
        }

        track.frames = trackJson["frames"];

        debug("all checked out, returning a valid Track");
        return Result<creatures::Track>{track};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while creating a Track from JSON: {}", e.what());
        warn(errorMessage);
        return Result<creatures::Track>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::AnimationMetadata> Database::animationMetadataFromJson(json animationMetadataJson) {

    debug("attempting to create an AnimationMetadata from JSON via animationMetadataFromJson()");
    debug("JSON: {}", animationMetadataJson.dump(4));

    try {

        auto metadata = AnimationMetadata();
        metadata.animation_id = animationMetadataJson["animation_id"];
        debug("animation_id: {}", metadata.animation_id);

        metadata.title = animationMetadataJson["title"];
        debug("title: {}", metadata.title);

        metadata.milliseconds_per_frame = animationMetadataJson["milliseconds_per_frame"];
        debug("milliseconds_per_frame: {}", metadata.milliseconds_per_frame);

        metadata.number_of_frames = animationMetadataJson["number_of_frames"];
        debug("number_of_frames: {}", metadata.number_of_frames);

        metadata.note = animationMetadataJson["note"];
        debug("note: {}", metadata.note);

        metadata.sound_file = animationMetadataJson["sound_file"];
        debug("sound_file: {}", metadata.sound_file);

        metadata.multitrack_audio = animationMetadataJson["multitrack_audio"];
        debug("multitrack_audio: {}", metadata.multitrack_audio);

        // Optional dialog-script provenance. Both absent for non-dialog
        // animations and for dialog animations rendered from inline turns.
        //
        // This parser is reached from both the trusted ingest path (JobWorker
        // writing its own snapshot) and the user-facing `/api/v1/animation`
        // upsert. We MUST validate strictly so a hostile client can't mint an
        // Animation claiming descent from a fake script, or attach a 100 MB
        // CoW blob — security review C1. Enforce the same bounds the
        // DialogScript validator uses.
        if (animationMetadataJson.contains("source_script_id") &&
            !animationMetadataJson["source_script_id"].is_null()) {
            if (!animationMetadataJson["source_script_id"].is_string()) {
                std::string errorMessage = "AnimationMetadata 'source_script_id' must be a string";
                warn(errorMessage);
                return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            const auto sid = animationMetadataJson["source_script_id"].get<std::string>();
            // Empty is fine — round-trip from a DB doc that never had provenance set.
            // Non-empty must be UUID-shaped to keep attacker strings out of logs/spans.
            if (!sid.empty() && !isUuidShape(sid)) {
                std::string errorMessage = "AnimationMetadata 'source_script_id' must be a UUID";
                warn(errorMessage);
                return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            metadata.source_script_id = sid;
        }
        if (animationMetadataJson.contains("source_script_turns") &&
            !animationMetadataJson["source_script_turns"].is_null()) {
            if (!animationMetadataJson["source_script_turns"].is_array()) {
                std::string errorMessage = "AnimationMetadata 'source_script_turns' must be an array";
                warn(errorMessage);
                return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            const auto &turnsJson = animationMetadataJson["source_script_turns"];
            if (turnsJson.size() > MAX_DIALOG_SCRIPT_TURNS) {
                std::string errorMessage = fmt::format("AnimationMetadata 'source_script_turns' has {} entries; max {}",
                                                       turnsJson.size(), MAX_DIALOG_SCRIPT_TURNS);
                warn(errorMessage);
                return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            metadata.source_script_turns.reserve(turnsJson.size());
            for (const auto &t : turnsJson) {
                if (!t.is_object()) {
                    std::string errorMessage = "AnimationMetadata 'source_script_turns' entries must be objects";
                    warn(errorMessage);
                    return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                DialogScriptTurn turn;
                if (!t.contains("creature_id") || !t["creature_id"].is_string()) {
                    std::string errorMessage = "source_script_turns entry missing string 'creature_id'";
                    warn(errorMessage);
                    return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                turn.creature_id = t["creature_id"].get<std::string>();
                if (!turn.creature_id.empty() && !isUuidShape(turn.creature_id)) {
                    std::string errorMessage = "source_script_turns entry 'creature_id' must be a UUID";
                    warn(errorMessage);
                    return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                if (!t.contains("text") || !t["text"].is_string()) {
                    std::string errorMessage = "source_script_turns entry missing string 'text'";
                    warn(errorMessage);
                    return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                turn.text = t["text"].get<std::string>();
                if (turn.text.size() > MAX_DIALOG_SCRIPT_TURN_TEXT) {
                    std::string errorMessage = fmt::format("source_script_turns entry 'text' is {} chars; max {}",
                                                           turn.text.size(), MAX_DIALOG_SCRIPT_TURN_TEXT);
                    warn(errorMessage);
                    return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                metadata.source_script_turns.push_back(std::move(turn));
            }
        }

        // Now let's validate it
        if (metadata.animation_id.empty()) {
            std::string errorMessage = "AnimationMetadata animation_id is empty";
            warn(errorMessage);
            return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        if (metadata.title.empty()) {
            std::string errorMessage = "AnimationMetadata title is empty";
            warn(errorMessage);
            return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        debug("all checked out, returning a valid AnimationMetadata");
        return Result<creatures::AnimationMetadata>{metadata};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while creating an AnimationMetadata from JSON: {}, (object: {})",
                                               e.what(), animationMetadataJson.dump(4));
        warn(errorMessage);
        return Result<creatures::AnimationMetadata>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::Animation> Database::animationFromJson(json animationJson) {

    debug("attempting to create an animation from JSON via animationFromJson()");

    try {

        auto animation = Animation();
        animation.id = animationJson["id"];
        debug("id: {}", animation.id);

        auto metadata = animationMetadataFromJson(animationJson["metadata"]);
        if (!metadata.isSuccess()) {
            auto error = metadata.getError().value();
            warn("Error while creating an AnimationMetadata from JSON: {}", error.getMessage());
            return Result<creatures::Animation>{ServerError(ServerError::InvalidData, error.getMessage())};
        }
        animation.metadata = metadata.getValue().value();

        // Add all of the tracks
        std::vector<json> tracksJson = animationJson["tracks"];
        for (const auto &trackJson : tracksJson) {
            auto track = trackFromJson(trackJson);
            if (!track.isSuccess()) {
                auto error = track.getError();
                warn("Error while creating a Track from JSON: {}", error->getMessage());
                return Result<creatures::Animation>{ServerError(ServerError::InvalidData, error->getMessage())};
            }
            animation.tracks.push_back(track.getValue().value());
        }

        return Result<creatures::Animation>{animation};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while creating an animation from JSON: {}", e.what());
        warn(errorMessage);
        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

/*
 * NOTE
 *
 * validateAnimationJson() is over in src/server/creature/helpers.cpp.
 *
 * The linker can't find it if it's here and heck if I know.
 *
 */

} // namespace creatures
