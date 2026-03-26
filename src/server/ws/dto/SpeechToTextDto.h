#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

class SpeechToTextResponseDto : public oatpp::DTO {
    DTO_INIT(SpeechToTextResponseDto, DTO)

    DTO_FIELD(String, status);
    DTO_FIELD(String, transcript);
    DTO_FIELD(Float64, audio_duration_sec);
    DTO_FIELD(Float64, transcription_time_ms);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
