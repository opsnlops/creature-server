
#pragma once

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


namespace creatures {

    struct FrameData {
        std::string id;
        std::string creature_id;
        std::string animation_id;
        std::vector<std::string> frames;  // The frame data will be base64 encoded strings

    };




#include OATPP_CODEGEN_BEGIN(DTO)

    class FrameDataDto : public oatpp::DTO {

        DTO_INIT(FrameDataDto, DTO /* extends */)

        DTO_FIELD(String, id);
        DTO_FIELD(String, creature_id);
        DTO_FIELD(String, animation_id);
        DTO_FIELD(List <String>, frames);

    };

#include OATPP_CODEGEN_END(DTO)


    std::shared_ptr<FrameDataDto> convertToDto(const FrameData &frameData);
    FrameData convertFromDto(const std::shared_ptr<FrameDataDto> &frameDataDTO);


}