
#include "spdlog/spdlog.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "server/database.h"

#include "util/Result.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

namespace creatures {

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

        track.creature_id = trackJson["creature_id"];
        debug("creature_id: {}", track.creature_id);

        if (track.creature_id.empty()) {
            std::string errorMessage = "Track creature_id is empty";
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
