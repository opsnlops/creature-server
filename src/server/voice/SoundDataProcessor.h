#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RhubarbData.h"
#include "model/Track.h"
#include "util/Result.h"

namespace creatures {

/**
 * @class SoundDataProcessor
 * @brief Processes Rhubarb Lip Sync data and integrates it into animation tracks
 *
 * This class converts time-based phoneme data from Rhubarb Lip Sync into
 * frame-based servo position data that can be used to animate creature mouths/jaws.
 */
class SoundDataProcessor {

  public:
    SoundDataProcessor() = default;
    ~SoundDataProcessor() = default;

    /**
     * @brief Process Rhubarb sound data into frame-based byte array
     *
     * Converts time-based phoneme cues into frame indices and generates
     * a byte array where each element represents the servo position for that frame.
     *
     * @param soundData The Rhubarb data containing phoneme cues
     * @param millisecondsPerFrame Animation frame duration in milliseconds (typically 1)
     * @param targetFrameCount Total number of frames in the animation
     * @return Vector of bytes representing servo positions for each frame
     */
    std::vector<uint8_t> processSoundData(const RhubarbSoundData &soundData, uint32_t millisecondsPerFrame,
                                          size_t targetFrameCount) const;

    /**
     * @brief Replace a specific axis in a track with processed sound data
     *
     * Takes Rhubarb phoneme data and replaces one axis (e.g., jaw servo) in all
     * frames of an animation track with the corresponding mouth position data.
     *
     * @param soundData The Rhubarb data to process
     * @param axisIndex Which axis (servo) to replace (0-based index)
     * @param track The animation track to modify
     * @param millisecondsPerFrame Animation frame duration in milliseconds
     * @return Result containing the modified track or an error
     */
    Result<Track> replaceAxisDataWithSoundData(const RhubarbSoundData &soundData, size_t axisIndex, const Track &track,
                                               uint32_t millisecondsPerFrame) const;

  private:
    /**
     * @brief Decode a base64-encoded frame into a byte vector
     * @param base64Frame The base64-encoded frame string
     * @return Vector of bytes representing servo positions
     */
    std::vector<uint8_t> decodeFrame(const std::string &base64Frame) const;

    /**
     * @brief Encode a byte vector into a base64-encoded string
     * @param frameData Vector of servo position bytes
     * @return Base64-encoded string
     */
    std::string encodeFrame(const std::vector<uint8_t> &frameData) const;
};

} // namespace creatures
