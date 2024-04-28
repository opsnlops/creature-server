

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>


#include "FrameData.h"

namespace creatures {

    // Convert FrameDataDTO to FrameData
    FrameData convertFromDTO(const std::shared_ptr<FrameDataDTO> &frameDataDTO) {
        FrameData frameData;  // Create an instance of FrameData
        frameData.id = frameDataDTO->id;  // Map fields
        frameData.creature_id = frameDataDTO->creature_id;
        frameData.animation_id = frameDataDTO->animation_id;

        // Ensure the list is initialized before iterating
        if (frameDataDTO->frames) {  // Check if the list is not null
            for (const auto &frame: *frameDataDTO->frames) {  // Correctly dereference and loop over the list
                frameData.frames.push_back(std::string(frame));  // Convert to std::string
            }
        }

        return frameData;  // Return the converted FrameData
    }

    // Convert FrameData to FrameDataDTO
    std::shared_ptr<FrameDataDTO> convertToDTO(const FrameData &frameData) {
        auto frameDataDTO = FrameDataDTO::createShared();  // Create a shared_ptr DTO
        frameDataDTO->id = frameData.id;  // Map fields
        frameDataDTO->creature_id = frameData.creature_id;
        frameDataDTO->animation_id = frameData.animation_id;
        frameDataDTO->frames = oatpp::List<oatpp::String>::createShared();

        for (const auto &frame: frameData.frames) {
            frameDataDTO->frames->emplace_back(frame);  // Copy the frames
        }

        return frameDataDTO.getPtr();
    }

}