
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures {

struct Track {
    std::string id;
    std::string creature_id;
    std::string animation_id;
    std::vector<std::string> frames; // The frame data will be base64 encoded strings
};

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * Data transfer object for FrameData
 */
class TrackDto : public oatpp::DTO {

    DTO_INIT(TrackDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "The ID of this track in the form of an UUID"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(creature_id) { info->description = "The ID of the creature this track belongs to"; }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(animation_id) { info->description = "The ID of the animation this track belongs to"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(frames) {
        info->description = "An array of base64 encoded strings that represent the frames of "
                            "the animation. Each frame is a 2D array of motion data.";
    }
    DTO_FIELD(List<String>, frames);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<TrackDto> convertToDto(const Track &track);
Track convertFromDto(const std::shared_ptr<TrackDto> &trackDto);

} // namespace creatures