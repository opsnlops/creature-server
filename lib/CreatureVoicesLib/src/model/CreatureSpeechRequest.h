
#pragma once

#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


namespace creatures :: voice {

    struct CreatureSpeechRequest {
        std::string creature_name;
        std::string title;
        std::string voice_id;
        std::string model_id;
        float stability;
        float similarity_boost;

        std::string text;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class CreatureSpeechRequestDto : public oatpp::DTO {

        DTO_INIT(CreatureSpeechRequestDto, DTO /* extends */)

        DTO_FIELD_INFO(creature_name) {
            info->description = "The name of the creature that will be talking. Used for metadata only.";
            info->required = false;
        }
        DTO_FIELD(String, creature_name);

        DTO_FIELD_INFO(title) {
            info->description = "The title of what the creature is say. Used for metadata only.";
            info->required = false;
        }
        DTO_FIELD(String, title);

        DTO_FIELD_INFO(voice_id) {
            info->description = "The 11labs voice_id to use for the speech synthesis.";
            info->required = true;
        }
        DTO_FIELD(String, voice_id);

        DTO_FIELD_INFO(model_id) {
            info->description = "The 11labs model_id to use for the speech synthesis.";
            info->required = true;
        }
        DTO_FIELD(String, model_id);

        DTO_FIELD_INFO(stability) {
            info->description = "The stability of the voice. A value between 0 and 1.";
            info->required = false;
        }
        DTO_FIELD(Float32, stability);

        DTO_FIELD_INFO(similarity_boost) {
            info->description = "The similarity boost of the voice. A value between 0 and 1.";
            info->required = false;
        }
        DTO_FIELD(Float32, similarity_boost);

        DTO_FIELD_INFO(text) {
            info->description = "The text that the creature will say.";
            info->required = true;
        }
        DTO_FIELD(String, text);



    };

#include OATPP_CODEGEN_END(DTO)


    oatpp::Object<CreatureSpeechRequestDto> convertToDto(const CreatureSpeechRequest &creatureSpeechRequest);
    CreatureSpeechRequest convertFromDto(const std::shared_ptr<CreatureSpeechRequestDto> &creatureSpeechRequestDto);


}

