#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace creatures {

/**
 * @struct RhubarbMetadata
 * @brief Metadata from Rhubarb Lip Sync JSON output
 */
struct RhubarbMetadata {
    std::string soundFile;
    double duration; // Duration in seconds

    /**
     * @brief Parse metadata from JSON object
     */
    static RhubarbMetadata fromJson(const nlohmann::json &j);
};

/**
 * @struct RhubarbMouthCue
 * @brief Represents a single mouth cue (phoneme) from Rhubarb output
 */
struct RhubarbMouthCue {
    double start;      // Start time in seconds
    double end;        // End time in seconds
    std::string value; // Phoneme letter (A, B, C, D, E, F, X)

    /**
     * @brief Convert phoneme letter to servo position value (0-255)
     * @return Servo position value for this phoneme
     */
    uint8_t toServoValue() const;

    /**
     * @brief Parse mouth cue from JSON object
     */
    static RhubarbMouthCue fromJson(const nlohmann::json &j);
};

/**
 * @struct RhubarbSoundData
 * @brief Complete Rhubarb Lip Sync output data
 */
struct RhubarbSoundData {
    RhubarbMetadata metadata;
    std::vector<RhubarbMouthCue> mouthCues;

    /**
     * @brief Parse complete Rhubarb JSON output
     * @param jsonString The JSON string from Rhubarb
     * @return Parsed RhubarbSoundData
     * @throws nlohmann::json::exception on parse error
     */
    static RhubarbSoundData fromJsonString(const std::string &jsonString);

    /**
     * @brief Parse from JSON object
     */
    static RhubarbSoundData fromJson(const nlohmann::json &j);
};

} // namespace creatures
