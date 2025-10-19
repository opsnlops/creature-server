#include "SoundDataProcessor.h"

#include <algorithm>
#include <base64.hpp>

#include "server/namespace-stuffs.h"
#include "util/helpers.h"

namespace creatures {

std::vector<uint8_t> SoundDataProcessor::processSoundData(const RhubarbSoundData &soundData,
                                                          uint32_t millisecondsPerFrame,
                                                          size_t targetFrameCount) const {

    debug("Processing Rhubarb data: duration {}ms, msPerFrame {}, targetFrames {}",
          soundData.metadata.duration * 1000.0, millisecondsPerFrame, targetFrameCount);

    // Initialize output array with zeros (mouth closed)
    std::vector<uint8_t> byteData(targetFrameCount, 0);

    // Map each cue's time range to frame indices
    for (const auto &cue : soundData.mouthCues) {
        // Convert time in seconds to frame indices
        // Clamp to valid range [0, targetFrameCount)
        const auto startFrameRaw =
            static_cast<int64_t>((cue.start * 1000.0) / static_cast<double>(millisecondsPerFrame));
        const auto endFrameRaw = static_cast<int64_t>((cue.end * 1000.0) / static_cast<double>(millisecondsPerFrame));

        const size_t startFrame = std::max(int64_t{0}, std::min(static_cast<int64_t>(targetFrameCount), startFrameRaw));
        const size_t endFrame =
            std::max(static_cast<int64_t>(startFrame), std::min(static_cast<int64_t>(targetFrameCount), endFrameRaw));

        // Fill the range with the phoneme's servo value
        if (endFrame > startFrame) {
            const uint8_t servoValue = cue.toServoValue();
            std::fill(byteData.begin() + startFrame, byteData.begin() + endFrame, servoValue);

            trace("Cue '{}' ({}-{}s) -> frames {}-{} = {}", cue.value, cue.start, cue.end, startFrame, endFrame,
                  servoValue);
        }
    }

    return byteData;
}

Result<Track> SoundDataProcessor::replaceAxisDataWithSoundData(const RhubarbSoundData &soundData, size_t axisIndex,
                                                               const Track &track,
                                                               uint32_t millisecondsPerFrame) const {

    info("Replacing axis {} on track {} using Rhubarb data", axisIndex, track.id);

    // Validate track has frames
    if (track.frames.empty()) {
        return ServerError(ServerError::InvalidData, "Track has no frames to replace");
    }

    // Decode first frame to determine width
    std::vector<uint8_t> firstFrameData = decodeFrame(track.frames[0]);
    const size_t frameWidth = firstFrameData.size();

    // Validate axis index is in bounds
    if (axisIndex >= frameWidth) {
        return ServerError(ServerError::InvalidData, "Axis index " + std::to_string(axisIndex) +
                                                         " out of bounds for frame width " +
                                                         std::to_string(frameWidth));
    }

    // Process the sound data into byte array
    const std::vector<uint8_t> newAxisData = processSoundData(soundData, millisecondsPerFrame, track.frames.size());

    // Create modified track
    Track modifiedTrack = track;

    // Replace the specified axis in each frame
    for (size_t frameIdx = 0; frameIdx < modifiedTrack.frames.size(); ++frameIdx) {
        // Decode the frame
        std::vector<uint8_t> frameData = decodeFrame(modifiedTrack.frames[frameIdx]);

        // Replace the axis value
        if (axisIndex < frameData.size()) {
            frameData[axisIndex] = newAxisData[frameIdx];
        }

        // Re-encode the frame
        modifiedTrack.frames[frameIdx] = encodeFrame(frameData);
    }

    info("Successfully replaced axis {} in {} frames", axisIndex, modifiedTrack.frames.size());

    return modifiedTrack;
}

std::vector<uint8_t> SoundDataProcessor::decodeFrame(const std::string &base64Frame) const {
    return decodeBase64(base64Frame);
}

std::string SoundDataProcessor::encodeFrame(const std::vector<uint8_t> &frameData) const {
    // Convert vector to string_view for base64 encoding
    std::string dataStr(frameData.begin(), frameData.end());
    return base64::to_base64(dataStr);
}

} // namespace creatures
