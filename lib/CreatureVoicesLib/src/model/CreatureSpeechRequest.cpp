
#include <string>

#include <oatpp/core/Types.hpp>

#include "CreatureSpeechRequest.h"

namespace creatures::voice {

    CreatureSpeechRequest convertFromDto(const std::shared_ptr<CreatureSpeechRequestDto> &creatureSpeechRequestDto) {
        CreatureSpeechRequest speechRequest;
        speechRequest.creature_name = creatureSpeechRequestDto->creature_name;
        speechRequest.title = creatureSpeechRequestDto->title;
        speechRequest.voice_id = creatureSpeechRequestDto->voice_id;
        speechRequest.model_id = creatureSpeechRequestDto->model_id;
        speechRequest.stability = creatureSpeechRequestDto->stability;
        speechRequest.similarity_boost = creatureSpeechRequestDto->similarity_boost;
        speechRequest.text = creatureSpeechRequestDto->text;

        return speechRequest;
    }

    // Convert this into its DTO
    oatpp::Object<CreatureSpeechRequestDto> convertToDto(const CreatureSpeechRequest &creatureSpeechRequest) {
        auto creatureSpeechRequestDto = CreatureSpeechRequestDto::createShared();
        creatureSpeechRequestDto->creature_name = creatureSpeechRequest.creature_name;
        creatureSpeechRequestDto->title = creatureSpeechRequest.title;
        creatureSpeechRequestDto->voice_id = creatureSpeechRequest.voice_id;
        creatureSpeechRequestDto->model_id = creatureSpeechRequest.model_id;
        creatureSpeechRequestDto->stability = creatureSpeechRequest.stability;
        creatureSpeechRequestDto->similarity_boost = creatureSpeechRequest.similarity_boost;
        creatureSpeechRequestDto->text = creatureSpeechRequest.text;

        return creatureSpeechRequestDto;
    }

}