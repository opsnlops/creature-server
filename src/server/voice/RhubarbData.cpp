#include "RhubarbData.h"

#include "server/namespace-stuffs.h"
#include <stdexcept>

namespace creatures {

// RhubarbMetadata implementation

RhubarbMetadata RhubarbMetadata::fromJson(const nlohmann::json &j) {
    RhubarbMetadata metadata;
    metadata.soundFile = j.at("soundFile").get<std::string>();
    metadata.duration = j.at("duration").get<double>();
    return metadata;
}

// RhubarbMouthCue implementation

uint8_t RhubarbMouthCue::toServoValue() const {
    // Map Rhubarb phoneme letters to servo positions (0-255)
    // These values match the mapping from the Swift implementation
    if (value == "A")
        return 5; // Nearly closed
    if (value == "B")
        return 180; // Wide open
    if (value == "C")
        return 240; // Very wide
    if (value == "D")
        return 255; // Maximum open
    if (value == "E")
        return 50; // Slightly open
    if (value == "F")
        return 20; // Mostly closed
    if (value == "X")
        return 0; // Rest/closed

    // Default to slightly open if unknown phoneme
    warn("Unknown phoneme value '{}', defaulting to 5", value);
    return 5;
}

RhubarbMouthCue RhubarbMouthCue::fromJson(const nlohmann::json &j) {
    RhubarbMouthCue cue;
    cue.start = j.at("start").get<double>();
    cue.end = j.at("end").get<double>();
    cue.value = j.at("value").get<std::string>();
    return cue;
}

// RhubarbSoundData implementation

RhubarbSoundData RhubarbSoundData::fromJsonString(const std::string &jsonString) {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonString);
        return fromJson(j);
    } catch (const nlohmann::json::exception &e) {
        error("Failed to parse Rhubarb JSON: {}", e.what());
        throw;
    }
}

RhubarbSoundData RhubarbSoundData::fromJson(const nlohmann::json &j) {
    RhubarbSoundData data;

    // Parse metadata
    if (j.contains("metadata")) {
        data.metadata = RhubarbMetadata::fromJson(j.at("metadata"));
    }

    // Parse mouth cues
    if (j.contains("mouthCues")) {
        const auto &cuesArray = j.at("mouthCues");
        data.mouthCues.reserve(cuesArray.size());

        for (const auto &cueJson : cuesArray) {
            data.mouthCues.push_back(RhubarbMouthCue::fromJson(cueJson));
        }
    }

    debug("Parsed Rhubarb data: {} cues, duration {}s", data.mouthCues.size(), data.metadata.duration);

    return data;
}

} // namespace creatures
