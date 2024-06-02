
#pragma once

#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


namespace creatures :: voice {

    struct Voice {
        std::string voiceId;
        std::string name;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class VoiceDto : public oatpp::DTO {

        DTO_INIT(VoiceDto, DTO /* extends */)

        DTO_FIELD_INFO(voice_id) {
            info->description = "The ID of the voice";
        }
        DTO_FIELD(String, voice_id);

        DTO_FIELD_INFO(name) {
            info->description = "The name of the voice";
        }
        DTO_FIELD(String, name);

    };

#include OATPP_CODEGEN_END(DTO)


    oatpp::Object<VoiceDto> convertToDto(const Voice &voice);
    Voice convertFromDto(const std::shared_ptr<VoiceDto> &voiceDto);


}

