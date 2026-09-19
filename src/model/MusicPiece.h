#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/voice/MusicTypes.h"
#include "util/Result.h"

namespace creatures {

/// Where a library version came from when it was composed against a dialog
/// take ("save to library" from the dialog editor). Absent for dialog-free
/// generations.
struct MusicVersionSourceDialog {
    std::string script_id;
    std::string dialog_cache_key;
    std::string dialog_generation_id;

    bool operator==(const MusicVersionSourceDialog &) const = default;
};

/// One generation promoted into a piece (#202). `id` is the music generation
/// id, which is also the promoted WAV's FILE_UID/TAKE. `sections` is the
/// editable form the next refinement starts from; `recipe` is the #200
/// recipe block (model, seed, song_id, the plan actually sent, …) read from
/// the WAV's provenance, kept opaque here because the API layer owns its
/// shape.
struct MusicPieceVersion {
    std::string id;
    std::string song_id;
    std::string sound_file; // permanent, relative to the sound root: music/<slug>--<id12>.wav
    std::string mp3_url;
    int64_t duration_ms{0};
    nlohmann::json recipe = nlohmann::json::object();
    std::vector<voice::MusicSection> sections;
    std::string base_version_id; // empty unless this version kept sections from another
    std::optional<MusicVersionSourceDialog> source_dialog;
    int64_t created_at{0};

    bool operator==(const MusicPieceVersion &) const = default;
};

/// A saved piece of music: title + notes + append-only versions.
/// `current_version_id` is the one that plays and that refinements start from.
struct MusicPiece {
    std::string id;
    std::string title;
    std::string notes;
    int64_t created_at{0};
    int64_t updated_at{0};
    std::string current_version_id;
    std::vector<MusicPieceVersion> versions;

    [[nodiscard]] const MusicPieceVersion *findVersion(std::string_view versionId) const;
    [[nodiscard]] const MusicPieceVersion *currentVersion() const { return findVersion(current_version_id); }

    bool operator==(const MusicPiece &) const = default;
};

inline constexpr std::size_t MAX_MUSIC_PIECE_TITLE_BYTES = 200;
inline constexpr std::size_t MAX_MUSIC_PIECE_NOTES_BYTES = 4000;
inline constexpr std::size_t MAX_MUSIC_PIECE_VERSIONS = 200;
inline constexpr std::size_t MAX_MUSIC_SECTIONS = voice::kMaxMusicPlanChunks;

/// Strict parser for one editable section (text, duration, styles,
/// adherence) with the composition-plan chunk limits. Shared by the API
/// boundary and persistence; `path` prefixes every error.
Result<voice::MusicSection> musicSectionFromJson(const nlohmann::json &json, const std::string &path);
Result<std::vector<voice::MusicSection>> musicSectionsFromJson(const nlohmann::json &json, const std::string &path);

nlohmann::json musicPieceVersionToJson(const MusicPieceVersion &version);
nlohmann::json musicPieceToJson(const MusicPiece &piece);

/// Persistence normalisation for a stored document (trusted, but every field
/// is still type-checked). `_id` must already be stripped.
Result<MusicPiece> musicPieceFromJson(const nlohmann::json &json);

} // namespace creatures
