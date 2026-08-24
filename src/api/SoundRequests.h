#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "model/JsonCodec.h"
#include "server/audio/SoundPathResolver.h"

namespace creatures::api {

inline constexpr std::size_t MAX_SOUND_CONTROL_REQUEST_BODY_BYTES = 4096;
inline constexpr std::size_t MAX_SOUND_UPLOAD_BODY_BYTES = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t MAX_SOUND_FILENAME_BYTES = 255;

struct PlaySoundRequest {
    std::string fileName;
};

struct GenerateLipSyncRequest {
    std::string soundFile;
    bool allowOverwrite{false};
};

inline Result<PlaySoundRequest> playSoundRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "sound play request", {"file_name"});
    if (!fields.isSuccess())
        return fields.getError().value();
    auto filename = json_codec::requiredString(json, "sound play request", "file_name", MAX_SOUND_FILENAME_BYTES);
    if (!filename.isSuccess())
        return filename.getError().value();
    if (!audio::isSafeSoundFilename(filename.getValue().value()))
        return json_codec::invalid<PlaySoundRequest>(
            "sound play request.file_name must be a safe filename without path components");
    return PlaySoundRequest{filename.getValue().value()};
}

inline Result<GenerateLipSyncRequest> generateLipSyncRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "lip sync request", {"sound_file", "allow_overwrite"});
    if (!fields.isSuccess())
        return fields.getError().value();
    auto filename = json_codec::requiredString(json, "lip sync request", "sound_file", MAX_SOUND_FILENAME_BYTES);
    if (!filename.isSuccess())
        return filename.getError().value();
    if (!audio::isSafeSoundFilename(filename.getValue().value()))
        return json_codec::invalid<GenerateLipSyncRequest>(
            "lip sync request.sound_file must be a safe filename without path components");
    bool allowOverwrite = false;
    if (json.contains("allow_overwrite")) {
        auto overwrite = json_codec::requiredBool(json, "lip sync request", "allow_overwrite");
        if (!overwrite.isSuccess())
            return overwrite.getError().value();
        allowOverwrite = overwrite.getValue().value();
    }
    return GenerateLipSyncRequest{filename.getValue().value(), allowOverwrite};
}

} // namespace creatures::api
