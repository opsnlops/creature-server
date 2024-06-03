
#pragma once

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


namespace creatures {

    struct Sound {
        std::string fileName;
        uint32_t size = 0;
        std::string transcript;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class SoundDto : public oatpp::DTO {

        DTO_INIT(SoundDto, DTO /* extends */)

        DTO_FIELD_INFO(file_name) {
            info->description = "The name of the sound file on the file system";
            info->required = true;
        }
        DTO_FIELD(String, file_name);


        DTO_FIELD_INFO(size) {
            info->description = "The size of the sound file in bytes";
            info->required = true;
        }
        DTO_FIELD(UInt32, size);

        DTO_FIELD_INFO(transcript) {
            info->description = "The file name of the sound file's transcript, if it has one";
            info->required = false;
        }
        DTO_FIELD(String, transcript);

    };

#include OATPP_CODEGEN_END(DTO)

    oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound);
    Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto);

}

