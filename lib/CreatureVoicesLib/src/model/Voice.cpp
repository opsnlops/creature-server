
#include <string>

#include <oatpp/core/Types.hpp>

#include "Voice.h"

namespace creatures::voice {

    Voice convertFromDto(const std::shared_ptr<VoiceDto> &voiceDto) {
        Voice voice;
        voice.voiceId = voiceDto->voice_id;
        voice.name = voiceDto->name;

        return voice;
    }

    // Convert this into its DTO
    oatpp::Object<VoiceDto> convertToDto(const Voice &voice) {
        auto voiceDto = VoiceDto::createShared();
        voiceDto->voice_id = voice.voiceId;
        voiceDto->name = voice.name;

        return voiceDto;
    }

}