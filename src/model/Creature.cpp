

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

#include <string>
#include <vector>

#include "Creature.h"

namespace creatures {

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
