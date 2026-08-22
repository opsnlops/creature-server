

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "AnimationMetadata.h"
#include "model/JsonCodec.h"
#include "model/Stage.h"
#include "util/helpers.h"

namespace creatures {

namespace {

Result<void> validatePlacementExtension(const nlohmann::json &value, std::string_view path, std::size_t depth = 0) {
    if (depth > 16) {
        return Result<void>{
            ServerError(ServerError::InvalidData, fmt::format("{} exceeds maximum extension nesting depth 16", path))};
    }
    if (value.is_string() && value.get_ref<const std::string &>().size() > MAX_ANIMATION_NOTE_BYTES) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                        fmt::format("{} string exceeds {} bytes", path, MAX_ANIMATION_NOTE_BYTES))};
    }
    if (value.is_array()) {
        if (value.size() > 256) {
            return Result<void>{ServerError(
                ServerError::InvalidData, fmt::format("{} array has {} entries; maximum is 256", path, value.size()))};
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            auto result = validatePlacementExtension(value[index], fmt::format("{}[{}]", path, index), depth + 1);
            if (!result.isSuccess())
                return result;
        }
    }
    if (value.is_object()) {
        if (value.size() > 64) {
            return Result<void>{ServerError(ServerError::InvalidData,
                                            fmt::format("{} object has {} fields; maximum is 64", path, value.size()))};
        }
        for (const auto &[key, child] : value.items()) {
            if (key.size() > 128 || key.starts_with('$') || key.find('.') != std::string::npos) {
                return Result<void>{
                    ServerError(ServerError::InvalidData, fmt::format("{} contains invalid persistence key {}", path,
                                                                      json_codec::diagnosticKey(key)))};
            }
            auto result = validatePlacementExtension(child, fmt::format("{}.{}", path, key), depth + 1);
            if (!result.isSuccess())
                return result;
        }
    }
    return Result<void>{};
}

} // namespace

nlohmann::json animationMetadataToJson(const AnimationMetadata &metadata) {
    nlohmann::json json{{"animation_id", metadata.animation_id},
                        {"title", metadata.title},
                        {"milliseconds_per_frame", metadata.milliseconds_per_frame},
                        {"sound_file", metadata.sound_file},
                        {"number_of_frames", metadata.number_of_frames},
                        {"multitrack_audio", metadata.multitrack_audio}};

    if (!metadata.note.empty()) {
        json["note"] = metadata.note;
    }
    if (!metadata.source_script_id.empty()) {
        json["source_script_id"] = metadata.source_script_id;
    }
    if (!metadata.source_script_turns.empty()) {
        auto turns = nlohmann::json::array();
        for (const auto &turn : metadata.source_script_turns) {
            turns.push_back({{"creature_id", turn.creature_id}, {"text", turn.text}});
        }
        json["source_script_turns"] = std::move(turns);
    }
    if (!metadata.source_stage_id.empty()) {
        json["source_stage_id"] = metadata.source_stage_id;
        json["source_stage_updated_at"] = metadata.source_stage_updated_at;
    }
    if (!metadata.source_stage_placements.empty()) {
        json["source_stage_placements"] = metadata.source_stage_placements;
    }
    if (metadata.render_seed != 0) {
        json["render_seed"] = metadata.render_seed;
    }
    if (!metadata.source_render_choices.empty()) {
        auto choices = nlohmann::json::array();
        for (const auto &choice : metadata.source_render_choices) {
            nlohmann::json choiceJson{{"creature_id", choice.creature_id},
                                      {"speech_loop_animation_id", choice.speech_loop_animation_id}};
            if (!choice.idle_animation_id.empty()) {
                choiceJson["idle_animation_id"] = choice.idle_animation_id;
            }
            if (choice.idle_start_offset != 0) {
                choiceJson["idle_start_offset"] = choice.idle_start_offset;
            }
            choices.push_back(std::move(choiceJson));
        }
        json["source_render_choices"] = std::move(choices);
    }

    return json;
}

Result<AnimationMetadata> animationMetadataFromJson(const nlohmann::json &json, std::string_view path,
                                                    bool allowTrustedAbsoluteSoundFile) {
    try {
        auto fields = json_codec::rejectUnknownFields(
            json, path,
            {"animation_id", "title", "milliseconds_per_frame", "note", "sound_file", "number_of_frames",
             "multitrack_audio", "source_script_id", "source_script_turns", "source_stage_id",
             "source_stage_updated_at", "source_stage_placements", "render_seed", "source_render_choices"});
        if (!fields.isSuccess())
            return Result<AnimationMetadata>{fields.getError().value()};

        auto animationId = json_codec::requiredString(json, path, "animation_id", 36);
        auto title = json_codec::requiredString(json, path, "title", MAX_ANIMATION_TITLE_BYTES);
        auto frameMillis = json_codec::requiredUnsigned<uint32_t>(json, path, "milliseconds_per_frame",
                                                                  MAX_ANIMATION_MILLISECONDS_PER_FRAME);
        auto note = json_codec::optionalString(json, path, "note", MAX_ANIMATION_NOTE_BYTES, true);
        auto soundFile = json_codec::requiredString(json, path, "sound_file", MAX_ANIMATION_SOUND_FILE_BYTES, true);
        auto frameCount =
            json_codec::requiredUnsigned<uint32_t>(json, path, "number_of_frames", MAX_ANIMATION_FRAMES_PER_TRACK);
        auto multitrack = json_codec::requiredBool(json, path, "multitrack_audio");
        if (!animationId.isSuccess())
            return Result<AnimationMetadata>{animationId.getError().value()};
        if (!title.isSuccess())
            return Result<AnimationMetadata>{title.getError().value()};
        if (!frameMillis.isSuccess())
            return Result<AnimationMetadata>{frameMillis.getError().value()};
        if (!note.isSuccess())
            return Result<AnimationMetadata>{note.getError().value()};
        if (!soundFile.isSuccess())
            return Result<AnimationMetadata>{soundFile.getError().value()};
        if (!frameCount.isSuccess())
            return Result<AnimationMetadata>{frameCount.getError().value()};
        if (!multitrack.isSuccess())
            return Result<AnimationMetadata>{multitrack.getError().value()};

        AnimationMetadata metadata;
        metadata.animation_id = animationId.getValue().value();
        metadata.title = title.getValue().value();
        metadata.milliseconds_per_frame = frameMillis.getValue().value();
        metadata.note = note.getValue().value().value_or(std::string{});
        metadata.sound_file = soundFile.getValue().value();
        if (!allowTrustedAbsoluteSoundFile && !metadata.sound_file.empty()) {
            const auto soundPath = std::filesystem::path(metadata.sound_file);
            const bool containsParent =
                std::any_of(soundPath.begin(), soundPath.end(), [](const auto &part) { return part == ".."; });
            if (metadata.sound_file.find('\0') != std::string::npos || soundPath.is_absolute() || containsParent ||
                soundPath != soundPath.lexically_normal()) {
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.sound_file must be a normalized relative path within the sound library", path));
            }
        }
        metadata.number_of_frames = frameCount.getValue().value();
        metadata.multitrack_audio = multitrack.getValue().value();
        if (!isUuidShape(metadata.animation_id))
            return json_codec::invalid<AnimationMetadata>(fmt::format("{}.animation_id must be a UUID", path));
        if (metadata.milliseconds_per_frame == 0)
            return json_codec::invalid<AnimationMetadata>(
                fmt::format("{}.milliseconds_per_frame must be greater than zero", path));

        auto scriptId = json_codec::optionalString(json, path, "source_script_id", 36);
        auto stageId = json_codec::optionalString(json, path, "source_stage_id", 36);
        auto stageUpdatedAt = json_codec::optionalInt64(json, path, "source_stage_updated_at");
        auto renderSeed = json_codec::optionalUnsigned<uint64_t>(json, path, "render_seed");
        if (!scriptId.isSuccess())
            return Result<AnimationMetadata>{scriptId.getError().value()};
        if (!stageId.isSuccess())
            return Result<AnimationMetadata>{stageId.getError().value()};
        if (!stageUpdatedAt.isSuccess())
            return Result<AnimationMetadata>{stageUpdatedAt.getError().value()};
        if (!renderSeed.isSuccess())
            return Result<AnimationMetadata>{renderSeed.getError().value()};
        metadata.source_script_id = scriptId.getValue().value().value_or(std::string{});
        metadata.source_stage_id = stageId.getValue().value().value_or(std::string{});
        metadata.source_stage_updated_at = stageUpdatedAt.getValue().value().value_or(0);
        metadata.render_seed = renderSeed.getValue().value().value_or(0);
        if (!metadata.source_script_id.empty() && !isUuidShape(metadata.source_script_id))
            return json_codec::invalid<AnimationMetadata>(fmt::format("{}.source_script_id must be a UUID", path));
        if (!metadata.source_stage_id.empty() && !isUuidShape(metadata.source_stage_id))
            return json_codec::invalid<AnimationMetadata>(fmt::format("{}.source_stage_id must be a UUID", path));
        if (metadata.source_stage_id.empty() != (metadata.source_stage_updated_at == 0))
            return json_codec::invalid<AnimationMetadata>(fmt::format(
                "{}.source_stage_id and source_stage_updated_at must both be present or both be absent", path));

        if (json.contains("source_script_turns")) {
            const auto &turns = json["source_script_turns"];
            if (!turns.is_array())
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_script_turns must be an array", path));
            if (turns.size() > MAX_DIALOG_SCRIPT_TURNS)
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_script_turns has {} entries; maximum is {}", path, turns.size(),
                                MAX_DIALOG_SCRIPT_TURNS));
            metadata.source_script_turns.reserve(turns.size());
            for (std::size_t index = 0; index < turns.size(); ++index) {
                const auto itemPath = fmt::format("{}.source_script_turns[{}]", path, index);
                auto itemFields = json_codec::rejectUnknownFields(turns[index], itemPath, {"creature_id", "text"});
                if (!itemFields.isSuccess())
                    return Result<AnimationMetadata>{itemFields.getError().value()};
                auto creatureId = json_codec::requiredString(turns[index], itemPath, "creature_id", 36);
                auto text =
                    json_codec::requiredString(turns[index], itemPath, "text", MAX_DIALOG_SCRIPT_TURN_TEXT, true);
                if (!creatureId.isSuccess())
                    return Result<AnimationMetadata>{creatureId.getError().value()};
                if (!text.isSuccess())
                    return Result<AnimationMetadata>{text.getError().value()};
                if (!isUuidShape(creatureId.getValue().value()))
                    return json_codec::invalid<AnimationMetadata>(
                        fmt::format("{}.creature_id must be a UUID", itemPath));
                metadata.source_script_turns.push_back({creatureId.getValue().value(), text.getValue().value()});
            }
        }

        if (json.contains("source_render_choices")) {
            const auto &choices = json["source_render_choices"];
            if (!choices.is_array())
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_render_choices must be an array", path));
            if (choices.size() > MAX_ANIMATION_RENDER_CHOICES)
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_render_choices has {} entries; maximum is {}", path, choices.size(),
                                MAX_ANIMATION_RENDER_CHOICES));
            metadata.source_render_choices.reserve(choices.size());
            std::unordered_set<std::string> seenCreatureIds;
            for (std::size_t index = 0; index < choices.size(); ++index) {
                const auto itemPath = fmt::format("{}.source_render_choices[{}]", path, index);
                auto itemFields = json_codec::rejectUnknownFields(
                    choices[index], itemPath,
                    {"creature_id", "speech_loop_animation_id", "idle_animation_id", "idle_start_offset"});
                if (!itemFields.isSuccess())
                    return Result<AnimationMetadata>{itemFields.getError().value()};
                auto creatureId = json_codec::requiredString(choices[index], itemPath, "creature_id", 36);
                auto speechId = json_codec::requiredString(choices[index], itemPath, "speech_loop_animation_id", 36);
                auto idleId = json_codec::optionalString(choices[index], itemPath, "idle_animation_id", 36);
                auto idleOffset = json_codec::optionalUnsigned<uint32_t>(choices[index], itemPath, "idle_start_offset");
                if (!creatureId.isSuccess())
                    return Result<AnimationMetadata>{creatureId.getError().value()};
                if (!speechId.isSuccess())
                    return Result<AnimationMetadata>{speechId.getError().value()};
                if (!idleId.isSuccess())
                    return Result<AnimationMetadata>{idleId.getError().value()};
                if (!idleOffset.isSuccess())
                    return Result<AnimationMetadata>{idleOffset.getError().value()};
                CreatureRenderChoice choice{creatureId.getValue().value(), speechId.getValue().value(),
                                            idleId.getValue().value().value_or(std::string{}),
                                            idleOffset.getValue().value().value_or(0)};
                if (!isUuidShape(choice.creature_id) || !isUuidShape(choice.speech_loop_animation_id) ||
                    (!choice.idle_animation_id.empty() && !isUuidShape(choice.idle_animation_id)))
                    return json_codec::invalid<AnimationMetadata>(
                        fmt::format("{} contains a non-UUID creature or animation id", itemPath));
                if (!seenCreatureIds.insert(choice.creature_id).second)
                    return json_codec::invalid<AnimationMetadata>(
                        fmt::format("{}.creature_id duplicates an earlier render choice", itemPath));
                metadata.source_render_choices.push_back(std::move(choice));
            }
        }

        if (json.contains("source_stage_placements")) {
            const auto &placements = json["source_stage_placements"];
            if (!placements.is_array())
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_stage_placements must be an array", path));
            if (placements.size() > MAX_STAGE_PLACEMENTS)
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_stage_placements has {} entries; maximum is {}", path, placements.size(),
                                MAX_STAGE_PLACEMENTS));
            const auto serializedSize = placements.dump().size();
            if (serializedSize > MAX_ANIMATION_STAGE_PLACEMENTS_BYTES)
                return json_codec::invalid<AnimationMetadata>(
                    fmt::format("{}.source_stage_placements is {} bytes; maximum is {}", path, serializedSize,
                                MAX_ANIMATION_STAGE_PLACEMENTS_BYTES));
            auto extensionResult =
                validatePlacementExtension(placements, fmt::format("{}.source_stage_placements", path));
            if (!extensionResult.isSuccess())
                return Result<AnimationMetadata>{extensionResult.getError().value()};
            std::unordered_set<std::string> seenCreatureIds;
            for (std::size_t index = 0; index < placements.size(); ++index) {
                const auto itemPath = fmt::format("{}.source_stage_placements[{}]", path, index);
                const auto &placement = placements[index];
                if (!placement.is_object())
                    return json_codec::invalid<AnimationMetadata>(fmt::format("{} must be an object", itemPath));
                auto creatureId = json_codec::requiredString(placement, itemPath, "creature_id", 36);
                if (!creatureId.isSuccess())
                    return Result<AnimationMetadata>{creatureId.getError().value()};
                if (!isUuidShape(creatureId.getValue().value()))
                    return json_codec::invalid<AnimationMetadata>(
                        fmt::format("{}.creature_id must be a UUID", itemPath));
                if (!seenCreatureIds.insert(creatureId.getValue().value()).second)
                    return json_codec::invalid<AnimationMetadata>(
                        fmt::format("{}.creature_id duplicates an earlier stage placement", itemPath));
                for (const std::string_view coordinate : {"x", "y", "z"}) {
                    auto iterator = placement.find(coordinate);
                    if (iterator == placement.end() || !iterator->is_number())
                        return json_codec::invalid<AnimationMetadata>(
                            fmt::format("{}.{} must be a number", itemPath, coordinate));
                    const auto value = iterator->get<double>();
                    if (!std::isfinite(value) || std::abs(value) > STAGE_COORD_LIMIT)
                        return json_codec::invalid<AnimationMetadata>(
                            fmt::format("{}.{} must be finite and between -{} and {}", itemPath, coordinate,
                                        STAGE_COORD_LIMIT, STAGE_COORD_LIMIT));
                }
                auto yaw = placement.find("yaw");
                if (yaw == placement.end() || !yaw->is_number() || !std::isfinite(yaw->get<double>()))
                    return json_codec::invalid<AnimationMetadata>(
                        fmt::format("{}.yaw must be a finite number", itemPath));
            }
            metadata.source_stage_placements = placements;
        }

        return Result<AnimationMetadata>{metadata};
    } catch (const nlohmann::json::exception &error) {
        return json_codec::invalid<AnimationMetadata>(fmt::format("{} is invalid JSON: {}", path, error.what()));
    } catch (const std::exception &error) {
        return json_codec::invalid<AnimationMetadata>(fmt::format("{} could not be parsed: {}", path, error.what()));
    }
}

} // namespace creatures
