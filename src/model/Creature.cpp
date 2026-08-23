

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>

#include <string>
#include <vector>

#include "Creature.h"

#include "model/JsonCodec.h"
#include "util/helpers.h"

namespace creatures {

namespace {

template <typename T> Result<Creature> forwardCreatureError(const Result<T> &result) {
    return Result<Creature>{result.getError().value()};
}

Result<std::vector<std::string>> parseAnimationIds(const nlohmann::json &json, std::string_view path,
                                                   std::string_view key) {
    const auto iterator = json.find(key);
    if (iterator == json.end())
        return Result<std::vector<std::string>>{std::vector<std::string>{}};
    if (!iterator->is_array())
        return json_codec::invalid<std::vector<std::string>>(fmt::format("{}.{} must be an array", path, key));
    if (iterator->size() > MAX_CREATURE_ANIMATION_IDS_PER_LIST) {
        return json_codec::invalid<std::vector<std::string>>(fmt::format(
            "{}.{} has {} entries; maximum is {}", path, key, iterator->size(), MAX_CREATURE_ANIMATION_IDS_PER_LIST));
    }

    std::vector<std::string> ids;
    ids.reserve(iterator->size());
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < iterator->size(); ++index) {
        const auto itemPath = fmt::format("{}.{}[{}]", path, key, index);
        const auto &value = (*iterator)[index];
        if (!value.is_string())
            return json_codec::invalid<std::vector<std::string>>(fmt::format("{} must be a string", itemPath));
        const auto id = value.get<std::string>();
        if (!isUuidShape(id))
            return json_codec::invalid<std::vector<std::string>>(fmt::format("{} must be a UUID", itemPath));
        if (!seen.insert(id).second)
            return json_codec::invalid<std::vector<std::string>>(fmt::format("{} duplicates UUID {}", itemPath, id));
        ids.push_back(id);
    }
    return Result<std::vector<std::string>>{ids};
}

Result<GazeAxis> parseGazeAxis(const nlohmann::json &json, std::string_view path, const Creature &creature) {
    auto fields =
        json_codec::rejectUnknownFields(json, path, {"input", "degrees_at_min", "degrees_at_max", "listening_amount"});
    if (!fields.isSuccess())
        return Result<GazeAxis>{fields.getError().value()};
    auto input = json_codec::requiredString(json, path, "input", MAX_INPUT_NAME_BYTES);
    if (!input.isSuccess())
        return Result<GazeAxis>{input.getError().value()};
    if (!inputSlotByName(creature, input.getValue().value()))
        return json_codec::invalid<GazeAxis>(fmt::format("{}.input must name one of creature.inputs", path));

    const auto minimum = json.find("degrees_at_min");
    const auto maximum = json.find("degrees_at_max");
    if (minimum == json.end() || !minimum->is_number())
        return json_codec::invalid<GazeAxis>(fmt::format("{}.degrees_at_min must be a number", path));
    if (maximum == json.end() || !maximum->is_number())
        return json_codec::invalid<GazeAxis>(fmt::format("{}.degrees_at_max must be a number", path));
    const float degreesAtMin = minimum->get<float>();
    const float degreesAtMax = maximum->get<float>();
    if (!std::isfinite(degreesAtMin) || !std::isfinite(degreesAtMax) || degreesAtMin == degreesAtMax)
        return json_codec::invalid<GazeAxis>(fmt::format("{} must define a finite, non-zero angular range", path));

    GazeAxis axis;
    axis.input = input.getValue().value();
    axis.degrees_at_min = degreesAtMin;
    axis.degrees_at_max = degreesAtMax;
    const auto listeningAmount = json.find("listening_amount");
    if (listeningAmount != json.end()) {
        if (!listeningAmount->is_number())
            return json_codec::invalid<GazeAxis>(fmt::format("{}.listening_amount must be a number", path));
        axis.listening_amount = listeningAmount->get<float>();
        if (!std::isfinite(axis.listening_amount) || axis.listening_amount < 0.0f || axis.listening_amount > 1.0f) {
            return json_codec::invalid<GazeAxis>(fmt::format("{}.listening_amount must be between 0 and 1", path));
        }
    }
    return Result<GazeAxis>{axis};
}

} // namespace

nlohmann::json creatureToJson(const Creature &creature) {
    nlohmann::json json{{"id", creature.id},
                        {"name", creature.name},
                        {"channel_offset", creature.channel_offset},
                        {"audio_channel", creature.audio_channel},
                        {"mouth_slot", creature.mouth_slot}};
    json["inputs"] = nlohmann::json::array();
    for (const auto &input : creature.inputs)
        json["inputs"].push_back(inputToJson(input));
    if (!creature.mouth_input.empty())
        json["mouth_input"] = creature.mouth_input;
    if (!creature.speech_loop_animation_ids.empty())
        json["speech_loop_animation_ids"] = creature.speech_loop_animation_ids;
    if (!creature.idle_animation_ids.empty())
        json["idle_animation_ids"] = creature.idle_animation_ids;
    if (creature.gaze) {
        auto gazeToJson = [](const GazeAxis &axis) {
            return nlohmann::json{{"input", axis.input},
                                  {"degrees_at_min", axis.degrees_at_min},
                                  {"degrees_at_max", axis.degrees_at_max},
                                  {"listening_amount", axis.listening_amount}};
        };
        nlohmann::json gaze = nlohmann::json::object();
        if (creature.gaze->pan)
            gaze["pan"] = gazeToJson(*creature.gaze->pan);
        if (creature.gaze->elevation)
            gaze["elevation"] = gazeToJson(*creature.gaze->elevation);
        if (creature.gaze->cock)
            gaze["cock"] = gazeToJson(*creature.gaze->cock);
        if (!gaze.empty())
            json["gaze"] = std::move(gaze);
    }
    return json;
}

Result<Creature> creatureFromJson(const nlohmann::json &json, std::string_view path) {
    try {
        auto object = json_codec::requireObject(json, path);
        if (!object.isSuccess())
            return forwardCreatureError(object);
        if (json.contains("_id")) {
            return json_codec::invalid<Creature>(fmt::format("{} must not contain reserved database field _id", path));
        }

        auto id = json_codec::requiredString(json, path, "id", 36);
        auto name = json_codec::requiredString(json, path, "name", MAX_CREATURE_NAME_BYTES);
        auto channelOffset = json_codec::requiredUnsigned<uint16_t>(json, path, "channel_offset", 511);
        auto audioChannel =
            json_codec::requiredUnsigned<uint16_t>(json, path, "audio_channel", MAX_CREATURE_AUDIO_CHANNEL);
        auto mouthSlot = json_codec::requiredUnsigned<uint8_t>(json, path, "mouth_slot", 255);
        if (!id.isSuccess())
            return forwardCreatureError(id);
        if (!name.isSuccess())
            return forwardCreatureError(name);
        if (!channelOffset.isSuccess())
            return forwardCreatureError(channelOffset);
        if (!audioChannel.isSuccess())
            return forwardCreatureError(audioChannel);
        if (!mouthSlot.isSuccess())
            return forwardCreatureError(mouthSlot);
        if (!isUuidShape(id.getValue().value()))
            return json_codec::invalid<Creature>(fmt::format("{}.id must be a UUID", path));
        if (audioChannel.getValue().value() == 0) {
            return json_codec::invalid<Creature>(
                fmt::format("{}.audio_channel must be in [1, {}]", path, MAX_CREATURE_AUDIO_CHANNEL));
        }

        const auto inputs = json.find("inputs");
        if (inputs != json.end() && !inputs->is_array())
            return json_codec::invalid<Creature>(fmt::format("{}.inputs must be an array", path));
        if (inputs != json.end() && inputs->size() > MAX_CREATURE_INPUTS) {
            return json_codec::invalid<Creature>(
                fmt::format("{}.inputs has {} entries; maximum is {}", path, inputs->size(), MAX_CREATURE_INPUTS));
        }

        Creature creature;
        creature.id = id.getValue().value();
        creature.name = name.getValue().value();
        creature.channel_offset = channelOffset.getValue().value();
        creature.audio_channel = audioChannel.getValue().value();
        creature.mouth_slot = mouthSlot.getValue().value();
        if (inputs != json.end())
            creature.inputs.reserve(inputs->size());
        std::unordered_set<std::string> names;
        std::vector<std::pair<uint32_t, uint32_t>> occupiedRanges;
        for (std::size_t index = 0; inputs != json.end() && index < inputs->size(); ++index) {
            const auto inputPath = fmt::format("{}.inputs[{}]", path, index);
            auto input = inputFromJson((*inputs)[index], inputPath);
            if (!input.isSuccess())
                return forwardCreatureError(input);
            const auto value = input.getValue().value();
            if (!names.insert(value.name).second)
                return json_codec::invalid<Creature>(fmt::format("{}.name duplicates another input", inputPath));
            const uint32_t start = value.slot;
            const uint32_t end = start + value.width;
            if (end > MAX_CREATURE_INPUT_SLOT_END) {
                return json_codec::invalid<Creature>(fmt::format("{} occupies slots through {}; maximum end is {}",
                                                                 inputPath, end - 1, MAX_CREATURE_INPUT_SLOT_END - 1));
            }
            const uint32_t universeEnd = static_cast<uint32_t>(creature.channel_offset) + end;
            if (universeEnd > MAX_CREATURE_INPUT_SLOT_END) {
                return json_codec::invalid<Creature>(
                    fmt::format("{} exceeds the DMX universe after channel_offset {}; maximum end is {}", inputPath,
                                creature.channel_offset, MAX_CREATURE_INPUT_SLOT_END - 1));
            }
            for (const auto &[otherStart, otherEnd] : occupiedRanges) {
                if (start < otherEnd && otherStart < end) {
                    return json_codec::invalid<Creature>(
                        fmt::format("{} overlaps another input slot range", inputPath));
                }
            }
            occupiedRanges.emplace_back(start, end);
            creature.inputs.push_back(value);
        }

        const auto mouthInput = json.find("mouth_input");
        if (mouthInput != json.end()) {
            if (!mouthInput->is_string())
                return json_codec::invalid<Creature>(fmt::format("{}.mouth_input must be a string", path));
            creature.mouth_input = mouthInput->get<std::string>();
            if (creature.mouth_input.empty() || creature.mouth_input.size() > MAX_INPUT_NAME_BYTES ||
                !inputSlotByName(creature, creature.mouth_input)) {
                return json_codec::invalid<Creature>(
                    fmt::format("{}.mouth_input must name one of creature.inputs", path));
            }
        }

        auto speech = parseAnimationIds(json, path, "speech_loop_animation_ids");
        auto idle = parseAnimationIds(json, path, "idle_animation_ids");
        if (!speech.isSuccess())
            return forwardCreatureError(speech);
        if (!idle.isSuccess())
            return forwardCreatureError(idle);
        creature.speech_loop_animation_ids = speech.getValue().value();
        creature.idle_animation_ids = idle.getValue().value();

        const auto gaze = json.find("gaze");
        if (gaze != json.end()) {
            auto gazeFields =
                json_codec::rejectUnknownFields(*gaze, fmt::format("{}.gaze", path), {"pan", "elevation", "cock"});
            if (!gazeFields.isSuccess())
                return forwardCreatureError(gazeFields);
            GazeConfig parsedGaze;
            for (const auto &[axisName, target] :
                 std::initializer_list<std::pair<const char *, std::optional<GazeAxis> *>>{
                     {"pan", &parsedGaze.pan}, {"elevation", &parsedGaze.elevation}, {"cock", &parsedGaze.cock}}) {
                const auto axis = gaze->find(axisName);
                if (axis == gaze->end())
                    continue;
                auto parsedAxis = parseGazeAxis(*axis, fmt::format("{}.gaze.{}", path, axisName), creature);
                if (!parsedAxis.isSuccess())
                    return forwardCreatureError(parsedAxis);
                *target = parsedAxis.getValue().value();
            }
            if (parsedGaze.pan || parsedGaze.elevation || parsedGaze.cock)
                creature.gaze = parsedGaze;
        }
        return Result<Creature>{creature};
    } catch (const nlohmann::json::exception &error) {
        return json_codec::invalid<Creature>(fmt::format("{} is invalid JSON: {}", path, error.what()));
    } catch (const std::exception &error) {
        return json_codec::invalid<Creature>(fmt::format("{} could not be parsed: {}", path, error.what()));
    }
}

// List of required fields
std::vector<std::string> creature_required_top_level_fields = {"id", "name", "audio_channel", "channel_offset",
                                                               "mouth_slot"};

std::vector<std::string> creature_required_input_fields = {"name", "slot", "width", "joystick_axis"};

std::optional<uint16_t> inputSlotByName(const Creature &creature, const std::string &inputName) {
    const auto it = std::find_if(creature.inputs.begin(), creature.inputs.end(),
                                 [&](const Input &input) { return input.name == inputName; });
    if (it == creature.inputs.end()) {
        return std::nullopt;
    }
    return it->slot;
}

uint16_t resolvedMouthSlot(const Creature &creature) {
    if (!creature.mouth_input.empty()) {
        if (const auto slot = inputSlotByName(creature, creature.mouth_input)) {
            return *slot;
        }
        // Named an input this creature doesn't have. The parser rejects that
        // on upload, so falling back to the raw number is the safest thing a
        // stored document that drifted can do.
        warn("Creature '{}' mouth_input '{}' does not name one of its inputs; falling back to mouth_slot {}",
             creature.name, creature.mouth_input, static_cast<int>(creature.mouth_slot));
    }
    return creature.mouth_slot;
}

std::optional<bool> mouthSlotMatchesBeak(const Creature &creature) {
    const auto beakSlot = inputSlotByName(creature, "beak");
    if (!beakSlot) {
        return std::nullopt;
    }
    return resolvedMouthSlot(creature) == *beakSlot;
}

} // namespace creatures
