
#pragma once

#include <cstdint>
#include <optional>
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
inline constexpr std::size_t MAX_DIALOG_MUSIC_PROMPT = 4100;
inline constexpr std::size_t MAX_DIALOG_MUSIC_SOUND_FILE = 1024;

/// One line in a saved dialog script — same shape as a render-time turn but
/// persisted so the script can be edited later and re-rendered. Mirrors
/// `creatures::ws::DialogTurnDto`; we redefine it in this namespace to keep
/// the model layer free of `ws::` types.
struct DialogScriptTurn {
    std::string creature_id;
    std::string text;
};

/// The voice take an author explicitly chose for a script (#131).
///
/// Mirrors DialogBackgroundMusic: an explicit, script-level choice the render
/// reads, rather than something reconstructed from provenance after the fact.
///
/// `dialog_cache_key` is the staleness test — sha256 of the turns this take
/// was accepted against. When it stops matching the script's current turns
/// the acceptance is stale: still reported, never auto-cleared. Nothing
/// chosen is ever silently un-chosen.
///
/// `sound_file` is the PROMOTED, permanent audio. Takes are generated as
/// ad-hoc sounds with a 24 h TTL; accepting moves the chosen one into the
/// real sounds directory so the script can reference it indefinitely, and
/// clearing moves it back. The sounds directory therefore holds at most one
/// take per script.
struct AcceptedVoice {
    std::string generation_id;
    std::string dialog_cache_key; // sha256 of the turns accepted against
    std::string sound_file;       // promoted, permanent, relative to the sound root
    int64_t accepted_at{0};       // wall-clock milliseconds since epoch

    bool operator==(const AcceptedVoice &) const = default;
};

struct DialogBackgroundMusic {
    std::string sound_file;    // permanent, relative-to-sounds WAV path
    std::string generation_id; // accepted server-side music generation UUID
    std::string prompt;        // exact ElevenLabs prompt used for the accepted take
    int64_t accepted_at{0};    // wall-clock milliseconds since epoch

    bool operator==(const DialogBackgroundMusic &) const = default;
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
    std::optional<DialogBackgroundMusic> background_music;
    /// The explicitly accepted voice take, if one has been chosen (#131).
    std::optional<AcceptedVoice> accepted_voice;
    /// Stage this script is normally rendered against (#119). Empty = none.
    /// A render request may override it; that's how you produce a travel
    /// rendition of a mainstage scene.
    std::string stage_id;
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

class AcceptedVoiceDto : public oatpp::DTO {

    DTO_INIT(AcceptedVoiceDto, DTO)

    DTO_FIELD_INFO(generation_id) { info->description = "UUID of the accepted dialog generation."; }
    DTO_FIELD(String, generation_id);

    DTO_FIELD_INFO(dialog_cache_key) {
        info->description = "sha256 of the turns this take was accepted against. When it no longer matches the "
                            "script's current turns the acceptance is stale — reported, never auto-cleared.";
    }
    DTO_FIELD(String, dialog_cache_key);

    DTO_FIELD_INFO(sound_file) {
        info->description = "Promoted permanent WAV for the accepted take, relative to the sound root. Outlives the "
                            "24h preview TTL, so the Console can always play it back.";
    }
    DTO_FIELD(String, sound_file);

    DTO_FIELD_INFO(accepted_at) { info->description = "Wall-clock milliseconds since epoch when it was accepted."; }
    DTO_FIELD(Int64, accepted_at);
};

class DialogBackgroundMusicDto : public oatpp::DTO {

    DTO_INIT(DialogBackgroundMusicDto, DTO)

    DTO_FIELD(String, sound_file);
    DTO_FIELD(String, generation_id);
    DTO_FIELD(String, prompt);
    DTO_FIELD(Int64, accepted_at);
};

class DialogScriptDto : public oatpp::DTO {

    DTO_INIT(DialogScriptDto, DTO /* extends */)

    DTO_FIELD_INFO(stage_id) {
        info->description = "Optional Stage UUID this script is normally rendered against, so re-renders stay "
                            "consistent. A render request may override it.";
        info->required = false;
    }
    DTO_FIELD(String, stage_id);

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

    DTO_FIELD_INFO(background_music) {
        info->description = "Accepted instrumental background music. sound_file always names the permanent WAV used "
                            "on dialog channel 17; MP3 is only an on-demand client rendition. Read-only: ordinary "
                            "script create/update requests cannot set or replace it; use music promotion or the "
                            "explicit clear-background-music endpoint.";
        info->required = false;
    }
    DTO_FIELD(Object<DialogBackgroundMusicDto>, background_music);

    DTO_FIELD_INFO(accepted_voice) {
        info->description = "The explicitly accepted voice take, if one has been chosen. Absent means no take has "
                            "been accepted — renders from this script are blocked until one is.";
        info->required = false;
    }
    DTO_FIELD(Object<AcceptedVoiceDto>, accepted_voice);

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
