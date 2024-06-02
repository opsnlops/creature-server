
#include <string>

#include <oatpp/core/Types.hpp>

#include "CreatureSpeechResponse.h"

namespace creatures::voice {

    CreatureSpeechResponse convertFromDto(const std::shared_ptr<CreatureSpeechResponseDto> &creatureSpeechResponseDto) {
        CreatureSpeechResponse speechResponse;
        speechResponse.success = creatureSpeechResponseDto->success;
        speechResponse.sound_file_name = creatureSpeechResponseDto->sound_file_name;
        speechResponse.transcript_file_name = creatureSpeechResponseDto->transcript_file_name;
        speechResponse.sound_file_size = creatureSpeechResponseDto->sound_file_size;

        return speechResponse;
    }

    // Convert this into its DTO
    oatpp::Object<CreatureSpeechResponseDto> convertToDto(const CreatureSpeechResponse &creatureSpeechResponse) {
        auto creatureSpeechResponseDto = CreatureSpeechResponseDto::createShared();
        creatureSpeechResponseDto->success = creatureSpeechResponse.success;
        creatureSpeechResponseDto->sound_file_name = creatureSpeechResponse.sound_file_name;
        creatureSpeechResponseDto->transcript_file_name = creatureSpeechResponse.transcript_file_name;
        creatureSpeechResponseDto->sound_file_size = creatureSpeechResponse.sound_file_size;

        return creatureSpeechResponseDto;
    }

}