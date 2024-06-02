
#pragma once

#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


namespace creatures :: voice {

    struct CreatureSpeechResponse {
        bool success;
        std::string sound_file_name;
        std::string transcript_file_name;
        uint32_t sound_file_size;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class CreatureSpeechResponseDto : public oatpp::DTO {

        DTO_INIT(CreatureSpeechResponseDto, DTO /* extends */)

        DTO_FIELD_INFO(success) {
            info->description = "Was the request successful";
        }
        DTO_FIELD(Boolean, success);

        DTO_FIELD_INFO(sound_file_name) {
            info->description = "Name of the audio file generated";
        }
        DTO_FIELD(String, sound_file_name);

        DTO_FIELD_INFO(transcript_file_name) {
            info->description = "Name of the transcript file generated";
        }
        DTO_FIELD(String, transcript_file_name);

        DTO_FIELD_INFO(sound_file_size) {
            info->description = "Size of the audio file generated";
        }
        DTO_FIELD(UInt32, sound_file_size);


    };

#include OATPP_CODEGEN_END(DTO)


    oatpp::Object<CreatureSpeechResponseDto> convertToDto(const CreatureSpeechResponse &speechResponse);
    CreatureSpeechResponse convertFromDto(const std::shared_ptr<CreatureSpeechResponseDto> &creatureSpeechResponseDto);

}

