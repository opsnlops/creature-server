#include "server/ws/dto/AdHocDtoUtils.h"

#include <filesystem>

#include "model/AnimationMetadata.h"
#include "model/Sound.h"
#include "util/helpers.h"

namespace fs = std::filesystem;

namespace creatures::ws {

oatpp::Object<AdHocSoundEntryDto> buildAdHocSoundEntryDto(const creatures::Animation &animation,
                                                          const std::chrono::system_clock::time_point &createdAt) {
    if (animation.metadata.sound_file.empty()) {
        return nullptr;
    }

    fs::path soundPath(animation.metadata.sound_file);
    auto dto = AdHocSoundEntryDto::createShared();
    dto->animation_id = animation.id;
    dto->sound_file = soundPath.string().c_str();
    dto->created_at = formatTimeISO8601(createdAt).c_str();

    creatures::Sound sound;
    sound.fileName = soundPath.filename().string();
    if (fs::exists(soundPath) && fs::is_regular_file(soundPath)) {
        sound.size = static_cast<uint32_t>(fs::file_size(soundPath));
    }

    auto transcriptPath = soundPath;
    transcriptPath.replace_extension(".txt");
    if (fs::exists(transcriptPath)) {
        sound.transcript = transcriptPath.filename().string();
    }

    auto lipsyncPath = soundPath;
    lipsyncPath.replace_extension(".json");
    if (fs::exists(lipsyncPath)) {
        sound.lipsync = lipsyncPath.filename().string();
    }

    dto->sound = convertSoundToDto(sound);
    return dto;
}

oatpp::Object<AdHocAnimationDto> buildAdHocAnimationDto(const creatures::Animation &animation,
                                                        const std::chrono::system_clock::time_point &createdAt) {
    auto dto = AdHocAnimationDto::createShared();
    dto->animation_id = animation.id;
    dto->metadata = creatures::convertToDto(animation.metadata);
    dto->created_at = formatTimeISO8601(createdAt).c_str();
    return dto;
}

} // namespace creatures::ws

