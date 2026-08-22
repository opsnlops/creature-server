
#include <string>
#include <unordered_set>
#include <vector>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/JsonCodec.h"
#include "model/Track.h"
#include "util/helpers.h"
#include <nlohmann/json.hpp>

namespace creatures {

nlohmann::json animationToJson(const Animation &animation) {
    auto tracks = nlohmann::json::array();
    for (const auto &track : animation.tracks) {
        tracks.push_back(trackToJson(track));
    }
    return {{"id", animation.id}, {"metadata", animationMetadataToJson(animation.metadata)}, {"tracks", tracks}};
}

Result<Animation> animationFromJson(const nlohmann::json &json, AnimationJsonSource source) {
    try {
        Result<void> fields =
            source == AnimationJsonSource::Persistence
                ? json_codec::rejectUnknownFields(json, "animation", {"id", "metadata", "tracks", "_id", "created_at"})
                : json_codec::rejectUnknownFields(json, "animation", {"id", "metadata", "tracks"});
        if (!fields.isSuccess())
            return Result<Animation>{fields.getError().value()};

        auto id = json_codec::requiredString(json, "animation", "id", 36);
        if (!id.isSuccess())
            return Result<Animation>{id.getError().value()};
        if (!isUuidShape(id.getValue().value()))
            return json_codec::invalid<Animation>("animation.id must be a UUID");

        const auto metadataIterator = json.find("metadata");
        if (metadataIterator == json.end())
            return json_codec::invalid<Animation>("animation.metadata is required");
        auto metadata = animationMetadataFromJson(*metadataIterator, "animation.metadata",
                                                  source == AnimationJsonSource::Persistence);
        if (!metadata.isSuccess())
            return Result<Animation>{metadata.getError().value()};
        if (metadata.getValue()->animation_id != id.getValue().value())
            return json_codec::invalid<Animation>("animation.metadata.animation_id must equal animation.id");

        const auto tracksIterator = json.find("tracks");
        if (tracksIterator == json.end())
            return json_codec::invalid<Animation>("animation.tracks is required");
        if (!tracksIterator->is_array())
            return json_codec::invalid<Animation>("animation.tracks must be an array");
        if (tracksIterator->size() > MAX_ANIMATION_TRACKS)
            return json_codec::invalid<Animation>(fmt::format("animation.tracks has {} entries; maximum is {}",
                                                              tracksIterator->size(), MAX_ANIMATION_TRACKS));
        if (!tracksIterator->empty() &&
            metadata.getValue()->number_of_frames > MAX_ANIMATION_TOTAL_FRAME_ENTRIES / tracksIterator->size())
            return json_codec::invalid<Animation>(fmt::format(
                "animation has more than {} total frame entries across all tracks", MAX_ANIMATION_TOTAL_FRAME_ENTRIES));

        Animation animation;
        animation.id = id.getValue().value();
        animation.metadata = metadata.getValue().value();
        const auto durationMs = static_cast<uint64_t>(animation.metadata.number_of_frames) *
                                static_cast<uint64_t>(animation.metadata.milliseconds_per_frame);
        if (durationMs > MAX_ANIMATION_DURATION_MS)
            return json_codec::invalid<Animation>(
                fmt::format("animation duration is {} ms; maximum is {}", durationMs, MAX_ANIMATION_DURATION_MS));
        animation.tracks.reserve(tracksIterator->size());
        std::size_t totalEncodedFrameBytes = 0;
        std::unordered_set<std::string> trackIds;
        std::unordered_set<std::string> targets;
        for (std::size_t index = 0; index < tracksIterator->size(); ++index) {
            auto track = trackFromJson((*tracksIterator)[index], fmt::format("animation.tracks[{}]", index));
            if (!track.isSuccess())
                return Result<Animation>{track.getError().value()};
            if (track.getValue()->animation_id != animation.id)
                return json_codec::invalid<Animation>(
                    fmt::format("animation.tracks[{}].animation_id must equal animation.id", index));
            if (!trackIds.insert(track.getValue()->id).second)
                return json_codec::invalid<Animation>(
                    fmt::format("animation.tracks[{}].id duplicates an earlier track", index));
            const auto target = !track.getValue()->creature_id.empty()
                                    ? fmt::format("creature:{}", track.getValue()->creature_id)
                                    : fmt::format("fixture:{}", track.getValue()->fixture_id);
            if (!targets.insert(target).second)
                return json_codec::invalid<Animation>(
                    fmt::format("animation.tracks[{}] duplicates an earlier target", index));
            if (track.getValue()->frames.size() != animation.metadata.number_of_frames)
                return json_codec::invalid<Animation>(
                    fmt::format("animation.tracks[{}].frames has {} entries but metadata.number_of_frames is {}", index,
                                track.getValue()->frames.size(), animation.metadata.number_of_frames));
            for (const auto &frame : track.getValue()->frames) {
                if (frame.size() > MAX_ANIMATION_TOTAL_ENCODED_FRAME_BYTES - totalEncodedFrameBytes)
                    return json_codec::invalid<Animation>(fmt::format("animation encoded frame data exceeds {} bytes",
                                                                      MAX_ANIMATION_TOTAL_ENCODED_FRAME_BYTES));
                totalEncodedFrameBytes += frame.size();
            }
            animation.tracks.push_back(track.getValue().value());
        }
        return Result<Animation>{animation};
    } catch (const nlohmann::json::exception &error) {
        return json_codec::invalid<Animation>(fmt::format("animation is invalid JSON: {}", error.what()));
    } catch (const std::exception &error) {
        return json_codec::invalid<Animation>(fmt::format("animation could not be parsed: {}", error.what()));
    }
}

} // namespace creatures
