

#include <string>
#include <vector>

#include <base64.hpp>

#include "Track.h"

#include <limits>

#include "model/JsonCodec.h"
#include "util/helpers.h"

namespace creatures {

nlohmann::json trackToJson(const Track &track) {
    nlohmann::json json{{"id", track.id}, {"animation_id", track.animation_id}, {"frames", track.frames}};
    if (!track.creature_id.empty()) {
        json["creature_id"] = track.creature_id;
    }
    if (!track.fixture_id.empty()) {
        json["fixture_id"] = track.fixture_id;
    }
    return json;
}

Result<Track> trackFromJson(const nlohmann::json &json, std::string_view path, bool allowLegacyNullOptionals) {
    try {
        auto fieldsResult =
            json_codec::rejectUnknownFields(json, path, {"id", "creature_id", "fixture_id", "animation_id", "frames"});
        if (!fieldsResult.isSuccess()) {
            return Result<Track>{fieldsResult.getError().value()};
        }

        Track track;
        auto idResult = json_codec::requiredString(json, path, "id", 36);
        auto animationIdResult = json_codec::requiredString(json, path, "animation_id", 36);
        auto creatureIdResult =
            json_codec::optionalString(json, path, "creature_id", 36, false, allowLegacyNullOptionals);
        auto fixtureIdResult =
            json_codec::optionalString(json, path, "fixture_id", 36, false, allowLegacyNullOptionals);
        if (!idResult.isSuccess())
            return Result<Track>{idResult.getError().value()};
        if (!animationIdResult.isSuccess())
            return Result<Track>{animationIdResult.getError().value()};
        if (!creatureIdResult.isSuccess())
            return Result<Track>{creatureIdResult.getError().value()};
        if (!fixtureIdResult.isSuccess())
            return Result<Track>{fixtureIdResult.getError().value()};

        track.id = idResult.getValue().value();
        track.animation_id = animationIdResult.getValue().value();
        track.creature_id = creatureIdResult.getValue().value().value_or(std::string{});
        track.fixture_id = fixtureIdResult.getValue().value().value_or(std::string{});

        if (!isUuidShape(track.id)) {
            return json_codec::invalid<Track>(fmt::format("{}.id must be a UUID", path));
        }
        if (!isUuidShape(track.animation_id)) {
            return json_codec::invalid<Track>(fmt::format("{}.animation_id must be a UUID", path));
        }
        if (!track.creature_id.empty() && !isUuidShape(track.creature_id)) {
            return json_codec::invalid<Track>(fmt::format("{}.creature_id must be a UUID", path));
        }
        if (!track.fixture_id.empty() && !isUuidShape(track.fixture_id)) {
            return json_codec::invalid<Track>(fmt::format("{}.fixture_id must be a UUID", path));
        }
        if (track.creature_id.empty() == track.fixture_id.empty()) {
            return json_codec::invalid<Track>(
                fmt::format("{} must contain exactly one of creature_id or fixture_id", path));
        }

        auto framesResult = json_codec::requiredArray(json, path, "frames", MAX_ANIMATION_FRAMES_PER_TRACK);
        if (!framesResult.isSuccess())
            return Result<Track>{framesResult.getError().value()};
        const auto &frames = framesResult.getValue()->get();
        track.frames.reserve(frames.size());
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const auto &frame = frames[index];
            if (!frame.is_string()) {
                return json_codec::invalid<Track>(fmt::format("{}.frames[{}] must be a string", path, index));
            }
            auto encoded = frame.get<std::string>();
            if (encoded.size() > MAX_ANIMATION_FRAME_ENCODED_BYTES) {
                return json_codec::invalid<Track>(fmt::format("{}.frames[{}] is {} bytes; maximum is {}", path, index,
                                                              encoded.size(), MAX_ANIMATION_FRAME_ENCODED_BYTES));
            }
            try {
                const auto decoded = base64::from_base64(encoded);
                if (decoded.empty()) {
                    return json_codec::invalid<Track>(
                        fmt::format("{}.frames[{}] must not decode to empty data", path, index));
                }
                if (decoded.size() > MAX_ANIMATION_FRAME_DECODED_BYTES) {
                    return json_codec::invalid<Track>(fmt::format("{}.frames[{}] decodes to {} bytes; maximum is {}",
                                                                  path, index, decoded.size(),
                                                                  MAX_ANIMATION_FRAME_DECODED_BYTES));
                }
            } catch (const std::exception &error) {
                return json_codec::invalid<Track>(
                    fmt::format("{}.frames[{}] is not valid base64: {}", path, index, error.what()));
            }
            track.frames.push_back(std::move(encoded));
        }
        return Result<Track>{track};
    } catch (const nlohmann::json::exception &error) {
        return json_codec::invalid<Track>(fmt::format("{} is invalid JSON: {}", path, error.what()));
    } catch (const std::exception &error) {
        return json_codec::invalid<Track>(fmt::format("{} could not be parsed: {}", path, error.what()));
    }
}

} // namespace creatures
