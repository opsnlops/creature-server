
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

/// Validation bounds shared between every code path that builds a DialogScript
/// from untrusted input. Defined here (alongside the model) so the controller's
/// up-front validator, the DB-layer parser, AND the JobWorker's
/// inline-turns / Animation-snapshot validators all enforce the same caps.
/// Caps are sized comfortably above any realistic show script and below any
/// size that'd be miserable to render — see the original fixture pattern.
inline constexpr std::size_t MAX_DIALOG_SCRIPT_TURNS = 200;
inline constexpr std::size_t MAX_DIALOG_SCRIPT_TURN_TEXT = 4096;
inline constexpr std::size_t MAX_DIALOG_SCRIPT_TITLE = 256;
inline constexpr std::size_t MAX_DIALOG_SCRIPT_NOTES = 16384;

/// One line in a saved dialog script — same shape as a render-time turn but
/// persisted so the script can be edited later and re-rendered. Mirrors
/// `creatures::ws::DialogTurnDto`; we redefine it in this namespace to keep
/// the model layer free of `ws::` types.
struct DialogScriptTurn {
    std::string creature_id;
    std::string text;
};

/// A saved multi-character dialog scene. Editable; CRUD'd via
/// `/api/v1/animation/dialog/script`. When the render endpoint receives a
/// `script_id`, it loads one of these and uses its turns; the resulting
/// Animation gets a `source_script_id` pointer + a copy-on-write snapshot
/// of these turns (see AnimationMetadata) so old animations stay readable
/// even after the script is edited.
struct DialogScript {
    std::string id;
    std::string title;
    std::string notes; // free-form, may be empty
    std::vector<DialogScriptTurn> turns;
    // Wall-clock milliseconds since epoch — server-managed, not honored from
    // the client. created_at is set on first insert and never changes.
    int64_t created_at{0};
    int64_t updated_at{0};
};

#include OATPP_CODEGEN_BEGIN(DTO)

class DialogScriptTurnDto : public oatpp::DTO {

    DTO_INIT(DialogScriptTurnDto, DTO /* extends */)

    DTO_FIELD_INFO(creature_id) { info->description = "UUID of the creature who speaks this turn."; }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(text) {
        info->description = "What the creature says. May contain inline ElevenLabs audio tags like [whispering] for "
                            "expressive delivery; tags are removed from the spoken text but kept in the model input.";
    }
    DTO_FIELD(String, text);
};

class DialogScriptDto : public oatpp::DTO {

    DTO_INIT(DialogScriptDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Script UUID. Server-generated on create."; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(title) { info->description = "Human-readable scene title."; }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(notes) {
        info->description = "Free-form notes attached to the script. Not shown to the audience — author's own.";
        info->required = false;
    }
    DTO_FIELD(String, notes);

    DTO_FIELD_INFO(turns) {
        info->description = "Ordered list of turns. Order matters — speaking order + ElevenLabs cross-speaker "
                            "reactivity order.";
    }
    DTO_FIELD(List<Object<DialogScriptTurnDto>>, turns);

    DTO_FIELD_INFO(created_at) {
        info->description = "Wall-clock milliseconds since Unix epoch when the script was first persisted. "
                            "Server-managed; ignored on PUT.";
    }
    DTO_FIELD(Int64, created_at);

    DTO_FIELD_INFO(updated_at) {
        info->description = "Wall-clock milliseconds since Unix epoch of the most recent edit. Server-managed; "
                            "ignored on PUT.";
    }
    DTO_FIELD(Int64, updated_at);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<DialogScriptDto> convertToDto(const DialogScript &script);
DialogScript convertFromDto(const std::shared_ptr<DialogScriptDto> &scriptDto);

/// Serialize a DialogScript to the JSON shape stored in MongoDB and returned
/// by the controller. Used by upsert + tests.
nlohmann::json dialogScriptToJson(const DialogScript &script);

} // namespace creatures
