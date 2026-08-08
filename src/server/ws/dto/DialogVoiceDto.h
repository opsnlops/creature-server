#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/// Request body for accepting a voice take (#131).
class AcceptVoiceRequestDto : public oatpp::DTO {

    DTO_INIT(AcceptVoiceRequestDto, DTO)

    DTO_FIELD_INFO(script_id) {
        info->description = "UUID of the dialog script the take is being accepted for.";
        info->required = true;
    }
    DTO_FIELD(String, script_id);

    DTO_FIELD_INFO(generation_id) {
        info->description = "UUID of the audition take being accepted.";
        info->required = true;
    }
    DTO_FIELD(String, generation_id);

    DTO_FIELD_INFO(dialog_cache_key) {
        info->description = "sha256 of the turns this take was auditioned against. Must match the script's current "
                            "turns — accepting a take for turns that have since changed is rejected rather than "
                            "silently stored as stale.";
        info->required = true;
    }
    DTO_FIELD(String, dialog_cache_key);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
