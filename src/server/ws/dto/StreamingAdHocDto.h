#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

class StreamingAdHocStartRequestDto : public oatpp::DTO {
    DTO_INIT(StreamingAdHocStartRequestDto, DTO)

    DTO_FIELD_INFO(creature_id) {
        info->description = "Creature ID to speak";
        info->required = true;
    }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(resume_playlist) {
        info->description = "Resume interrupted playlist after all speech finishes";
        info->required = false;
    }
    DTO_FIELD(Boolean, resume_playlist) = true;
};

class StreamingAdHocStartResponseDto : public oatpp::DTO {
    DTO_INIT(StreamingAdHocStartResponseDto, DTO)

    DTO_FIELD(String, session_id);
    DTO_FIELD(String, status);
    DTO_FIELD(String, message);
};

class StreamingAdHocTextRequestDto : public oatpp::DTO {
    DTO_INIT(StreamingAdHocTextRequestDto, DTO)

    DTO_FIELD_INFO(session_id) {
        info->description = "Session ID from the start call";
        info->required = true;
    }
    DTO_FIELD(String, session_id);

    DTO_FIELD_INFO(text) {
        info->description = "A sentence or text chunk to add to the speech";
        info->required = true;
    }
    DTO_FIELD(String, text);
};

class StreamingAdHocTextResponseDto : public oatpp::DTO {
    DTO_INIT(StreamingAdHocTextResponseDto, DTO)

    DTO_FIELD(String, session_id);
    DTO_FIELD(String, status);
    DTO_FIELD(Int32, chunks_received);
};

class StreamingAdHocFinishRequestDto : public oatpp::DTO {
    DTO_INIT(StreamingAdHocFinishRequestDto, DTO)

    DTO_FIELD_INFO(session_id) {
        info->description = "Session ID from the start call";
        info->required = true;
    }
    DTO_FIELD(String, session_id);
};

class StreamingAdHocFinishResponseDto : public oatpp::DTO {
    DTO_INIT(StreamingAdHocFinishResponseDto, DTO)

    DTO_FIELD(String, session_id);
    DTO_FIELD(String, status);
    DTO_FIELD(String, message);
    DTO_FIELD(String, animation_id);
    DTO_FIELD(Boolean, playback_triggered);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
