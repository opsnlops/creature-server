#include "model/MusicPiece.h"

#include <algorithm>

#include <fmt/format.h>

#include "model/JsonCodec.h"

namespace creatures {

const MusicPieceVersion *MusicPiece::findVersion(std::string_view versionId) const {
    const auto it = std::find_if(versions.begin(), versions.end(),
                                 [&](const MusicPieceVersion &version) { return version.id == versionId; });
    return it == versions.end() ? nullptr : &*it;
}

namespace {

Result<std::vector<std::string>> styleListFromJson(const nlohmann::json &json, const std::string &path,
                                                   std::string_view key, bool required) {
    using ListResult = Result<std::vector<std::string>>;
    if (!required && !json.contains(key))
        return ListResult{std::vector<std::string>{}};
    auto array = json_codec::requiredArray(json, path, key, voice::kMaxMusicStyles);
    if (!array.isSuccess())
        return ListResult{array.getError().value()};
    std::vector<std::string> styles;
    std::size_t index = 0;
    for (const auto &item : array.getValue().value().get()) {
        const auto itemPath = fmt::format("{}.{}[{}]", path, key, index++);
        if (!item.is_string())
            return json_codec::invalid<std::vector<std::string>>(fmt::format("{} must be a string", itemPath));
        auto value = item.get<std::string>();
        if (value.empty() || value.size() > voice::kMaxMusicStyleBytes)
            return json_codec::invalid<std::vector<std::string>>(
                fmt::format("{} must be 1-{} bytes", itemPath, voice::kMaxMusicStyleBytes));
        styles.push_back(std::move(value));
    }
    return ListResult{std::move(styles)};
}

} // namespace

Result<voice::MusicSection> musicSectionFromJson(const nlohmann::json &json, const std::string &path) {
    using SectionResult = Result<voice::MusicSection>;
    auto fields = json_codec::rejectUnknownFields(
        json, path, {"text", "duration_ms", "positive_styles", "negative_styles", "context_adherence"});
    if (!fields.isSuccess())
        return SectionResult{fields.getError().value()};
    auto text = json_codec::requiredString(json, path, "text", voice::kMaxMusicChunkTextBytes, true);
    auto duration = json_codec::requiredInt64(json, path, "duration_ms", voice::kMinMusicChunkDurationMs,
                                              voice::kMaxMusicChunkDurationMs);
    auto positive = styleListFromJson(json, path, "positive_styles", true);
    auto negative = styleListFromJson(json, path, "negative_styles", false);
    auto adherence = json_codec::optionalString(json, path, "context_adherence", 16);
    if (!text.isSuccess())
        return SectionResult{text.getError().value()};
    if (!duration.isSuccess())
        return SectionResult{duration.getError().value()};
    if (!positive.isSuccess())
        return SectionResult{positive.getError().value()};
    if (!negative.isSuccess())
        return SectionResult{negative.getError().value()};
    if (!adherence.isSuccess())
        return SectionResult{adherence.getError().value()};
    voice::MusicSection section;
    section.text = text.getValue().value();
    section.durationMs = duration.getValue().value();
    section.positiveStyles = positive.getValue().value();
    section.negativeStyles = negative.getValue().value();
    section.contextAdherence = adherence.getValue().value().value_or("high");
    if (!voice::isSupportedContextAdherence(section.contextAdherence))
        return json_codec::invalid<voice::MusicSection>(path + ".context_adherence must be 'low', 'medium', or 'high'");
    return SectionResult{std::move(section)};
}

Result<std::vector<voice::MusicSection>> musicSectionsFromJson(const nlohmann::json &json, const std::string &path) {
    using ListResult = Result<std::vector<voice::MusicSection>>;
    if (!json.is_array())
        return json_codec::invalid<std::vector<voice::MusicSection>>(path + " must be an array");
    if (json.empty())
        return json_codec::invalid<std::vector<voice::MusicSection>>(path + " must not be empty");
    if (json.size() > MAX_MUSIC_SECTIONS)
        return json_codec::invalid<std::vector<voice::MusicSection>>(
            fmt::format("{} has {} entries; maximum is {}", path, json.size(), MAX_MUSIC_SECTIONS));
    std::vector<voice::MusicSection> sections;
    std::size_t index = 0;
    for (const auto &item : json) {
        auto section = musicSectionFromJson(item, fmt::format("{}[{}]", path, index++));
        if (!section.isSuccess())
            return ListResult{section.getError().value()};
        sections.push_back(section.getValue().value());
    }
    const auto total = voice::musicSectionsTotalMs(sections);
    if (total < voice::kMinMusicLengthMs || total > voice::kMaxMusicLengthMs)
        return json_codec::invalid<std::vector<voice::MusicSection>>(fmt::format(
            "{} total {} ms; must be {}-{} ms", path, total, voice::kMinMusicLengthMs, voice::kMaxMusicLengthMs));
    if (const auto bytes = voice::musicSectionsToJson(sections).dump().size(); bytes > voice::kMaxMusicPlanJsonBytes)
        return json_codec::invalid<std::vector<voice::MusicSection>>(
            fmt::format("{} serializes to {} bytes; maximum is {}", path, bytes, voice::kMaxMusicPlanJsonBytes));
    return ListResult{std::move(sections)};
}

nlohmann::json musicPieceVersionToJson(const MusicPieceVersion &version) {
    nlohmann::json json{{"id", version.id},
                        {"song_id", version.song_id},
                        {"sound_file", version.sound_file},
                        {"mp3_url", version.mp3_url},
                        {"duration_ms", version.duration_ms},
                        {"recipe", version.recipe.is_object() ? version.recipe : nlohmann::json::object()},
                        {"sections", voice::musicSectionsToJson(version.sections)},
                        {"created_at", version.created_at}};
    if (!version.base_version_id.empty())
        json["base_version_id"] = version.base_version_id;
    if (version.source_dialog)
        json["source_dialog"] = {{"script_id", version.source_dialog->script_id},
                                 {"dialog_cache_key", version.source_dialog->dialog_cache_key},
                                 {"dialog_generation_id", version.source_dialog->dialog_generation_id}};
    return json;
}

nlohmann::json musicPieceToJson(const MusicPiece &piece) {
    auto versions = nlohmann::json::array();
    for (const auto &version : piece.versions)
        versions.push_back(musicPieceVersionToJson(version));
    return {{"id", piece.id},
            {"title", piece.title},
            {"notes", piece.notes},
            {"created_at", piece.created_at},
            {"updated_at", piece.updated_at},
            {"current_version_id", piece.current_version_id},
            {"versions", std::move(versions)}};
}

namespace {

Result<MusicPieceVersion> versionFromJson(const nlohmann::json &json, const std::string &path) {
    using VersionResult = Result<MusicPieceVersion>;
    auto fields = json_codec::rejectUnknownFields(json, path,
                                                  {"id", "song_id", "sound_file", "mp3_url", "duration_ms", "recipe",
                                                   "sections", "base_version_id", "source_dialog", "created_at"});
    if (!fields.isSuccess())
        return VersionResult{fields.getError().value()};
    MusicPieceVersion version;
    auto id = json_codec::requiredString(json, path, "id", 64);
    auto songId = json_codec::optionalString(json, path, "song_id", voice::kMaxMusicSongIdBytes, true);
    auto soundFile = json_codec::requiredString(json, path, "sound_file", 512);
    auto mp3Url = json_codec::requiredString(json, path, "mp3_url", 512);
    auto duration = json_codec::requiredInt64(json, path, "duration_ms", 0, voice::kMaxMusicLengthMs);
    auto baseVersion = json_codec::optionalString(json, path, "base_version_id", 64);
    auto createdAt = json_codec::requiredInt64(json, path, "created_at", 1);
    if (!id.isSuccess())
        return VersionResult{id.getError().value()};
    if (!songId.isSuccess())
        return VersionResult{songId.getError().value()};
    if (!soundFile.isSuccess())
        return VersionResult{soundFile.getError().value()};
    if (!mp3Url.isSuccess())
        return VersionResult{mp3Url.getError().value()};
    if (!duration.isSuccess())
        return VersionResult{duration.getError().value()};
    if (!baseVersion.isSuccess())
        return VersionResult{baseVersion.getError().value()};
    if (!createdAt.isSuccess())
        return VersionResult{createdAt.getError().value()};
    version.id = id.getValue().value();
    version.song_id = songId.getValue().value().value_or("");
    version.sound_file = soundFile.getValue().value();
    version.mp3_url = mp3Url.getValue().value();
    version.duration_ms = duration.getValue().value();
    version.base_version_id = baseVersion.getValue().value().value_or("");
    version.created_at = createdAt.getValue().value();
    if (json.contains("recipe")) {
        if (!json["recipe"].is_object())
            return json_codec::invalid<MusicPieceVersion>(path + ".recipe must be an object");
        version.recipe = json["recipe"];
    }
    if (!json.contains("sections"))
        return json_codec::invalid<MusicPieceVersion>(path + ".sections is required");
    auto sections = musicSectionsFromJson(json["sections"], path + ".sections");
    if (!sections.isSuccess())
        return VersionResult{sections.getError().value()};
    version.sections = sections.getValue().value();
    if (json.contains("source_dialog") && !json["source_dialog"].is_null()) {
        const auto &source = json["source_dialog"];
        const auto sourcePath = path + ".source_dialog";
        auto sourceFields = json_codec::rejectUnknownFields(source, sourcePath,
                                                            {"script_id", "dialog_cache_key", "dialog_generation_id"});
        if (!sourceFields.isSuccess())
            return VersionResult{sourceFields.getError().value()};
        auto scriptId = json_codec::requiredString(source, sourcePath, "script_id", 64);
        auto cacheKey = json_codec::requiredString(source, sourcePath, "dialog_cache_key", 64);
        auto generationId = json_codec::requiredString(source, sourcePath, "dialog_generation_id", 64);
        if (!scriptId.isSuccess())
            return VersionResult{scriptId.getError().value()};
        if (!cacheKey.isSuccess())
            return VersionResult{cacheKey.getError().value()};
        if (!generationId.isSuccess())
            return VersionResult{generationId.getError().value()};
        version.source_dialog = MusicVersionSourceDialog{scriptId.getValue().value(), cacheKey.getValue().value(),
                                                         generationId.getValue().value()};
    }
    return VersionResult{std::move(version)};
}

} // namespace

Result<MusicPiece> musicPieceFromJson(const nlohmann::json &json) {
    using PieceResult = Result<MusicPiece>;
    constexpr const char *path = "music piece";
    auto fields = json_codec::rejectUnknownFields(
        json, path, {"id", "title", "notes", "created_at", "updated_at", "current_version_id", "versions"});
    if (!fields.isSuccess())
        return PieceResult{fields.getError().value()};
    auto id = json_codec::requiredString(json, path, "id", 64);
    auto title = json_codec::requiredString(json, path, "title", MAX_MUSIC_PIECE_TITLE_BYTES);
    auto notes = json_codec::optionalString(json, path, "notes", MAX_MUSIC_PIECE_NOTES_BYTES, true, true);
    auto createdAt = json_codec::requiredInt64(json, path, "created_at", 1);
    auto updatedAt = json_codec::requiredInt64(json, path, "updated_at", 1);
    auto current = json_codec::requiredString(json, path, "current_version_id", 64);
    auto versionsJson = json_codec::requiredArray(json, path, "versions", MAX_MUSIC_PIECE_VERSIONS, 1);
    if (!id.isSuccess())
        return PieceResult{id.getError().value()};
    if (!title.isSuccess())
        return PieceResult{title.getError().value()};
    if (!notes.isSuccess())
        return PieceResult{notes.getError().value()};
    if (!createdAt.isSuccess())
        return PieceResult{createdAt.getError().value()};
    if (!updatedAt.isSuccess())
        return PieceResult{updatedAt.getError().value()};
    if (!current.isSuccess())
        return PieceResult{current.getError().value()};
    if (!versionsJson.isSuccess())
        return PieceResult{versionsJson.getError().value()};
    MusicPiece piece;
    piece.id = id.getValue().value();
    piece.title = title.getValue().value();
    piece.notes = notes.getValue().value().value_or("");
    piece.created_at = createdAt.getValue().value();
    piece.updated_at = updatedAt.getValue().value();
    piece.current_version_id = current.getValue().value();
    std::size_t index = 0;
    for (const auto &item : versionsJson.getValue().value().get()) {
        auto version = versionFromJson(item, fmt::format("{}.versions[{}]", path, index++));
        if (!version.isSuccess())
            return PieceResult{version.getError().value()};
        if (piece.findVersion(version.getValue()->id))
            return json_codec::invalid<MusicPiece>(
                fmt::format("{}.versions has duplicate id {}", path, version.getValue()->id));
        piece.versions.push_back(version.getValue().value());
    }
    if (!piece.currentVersion())
        return json_codec::invalid<MusicPiece>("music piece.current_version_id does not name a version");
    return PieceResult{std::move(piece)};
}

} // namespace creatures
