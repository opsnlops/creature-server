#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/DialogScript.h"

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/// Request body for POST/PUT /api/v1/animation/dialog/script[/{id}].
/// `id`, `created_at`, `updated_at` are server-managed and rejected here so
/// clients can't spoof them. `id` for PUT comes from the URL path; POST gets
/// a server-generated UUID.
class UpsertDialogScriptRequestDto : public oatpp::DTO {

    DTO_INIT(UpsertDialogScriptRequestDto, DTO)

    DTO_FIELD_INFO(title) {
        info->description = "Human-readable scene title. Required, non-empty.";
        info->required = true;
    }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(notes) {
        info->description = "Free-form notes attached to the script. Optional.";
        info->required = false;
    }
    DTO_FIELD(String, notes);

    DTO_FIELD_INFO(turns) {
        info->description = "Ordered list of turns. Required, non-empty. Each turn must have a non-empty creature_id "
                            "and text.";
        info->required = true;
    }
    DTO_FIELD(List<Object<creatures::DialogScriptTurnDto>>, turns);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
