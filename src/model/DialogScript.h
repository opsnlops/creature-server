
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "model/DialogScriptTypes.h"
#include "server/namespace-stuffs.h"
#include <nlohmann/json.hpp>

namespace creatures {

/// Validation bounds shared between every code path that builds a DialogScript
/// from untrusted input. Defined here (alongside the model) so the controller's
/// up-front validator, the DB-layer parser, AND the JobWorker's
/// inline-turns / Animation-snapshot validators all enforce the same caps.
/// Caps are sized comfortably above any realistic show script and below any
/// size that'd be miserable to render — see the original fixture pattern.
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

/// The two `source_dialog_*` fields record which VOICE take the music was
/// composed against (#136). Music is fitted to a specific performance's
/// timing and length, so when the accepted voice changes, accepted music
/// that predates it is describing audio that will never render again.
/// Without this the Console can only report candidates as stale — the
/// already-accepted card sits green and silent, which is the worst kind of
/// wrong on the surface where scenes are actually built.
///
/// Both are empty on music accepted before this was recorded. That's
/// reported as "no verdict", never as "fresh" — an unknown provenance must
/// not read as a passing one.
struct DialogBackgroundMusic {
    std::string sound_file;                  // permanent, relative-to-sounds WAV path
    std::string generation_id;               // accepted server-side music generation UUID
    std::string prompt;                      // exact ElevenLabs prompt used for the accepted take
    int64_t accepted_at{0};                  // wall-clock milliseconds since epoch
    std::string source_dialog_generation_id; // voice take this was composed against
    std::string source_dialog_cache_key;     // sha256 of the turns at composition time

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

/// Serialize a DialogScript to the JSON shape stored in MongoDB and returned
/// by the controller. Used by upsert + tests.
nlohmann::json dialogScriptToJson(const DialogScript &script);

} // namespace creatures
