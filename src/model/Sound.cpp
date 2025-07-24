

#include <string>

#include "Sound.h"

namespace creatures {

Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto) {
    Sound sound;
    sound.fileName = soundDto->file_name;
    sound.size = soundDto->size;
    sound.transcript = soundDto->transcript;

    return sound;
}

oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound) {
    auto soundDto = SoundDto::createShared();
    soundDto->file_name = sound.fileName;
    soundDto->size = sound.size;
    soundDto->transcript = sound.transcript;

    return soundDto;
}

} // namespace creatures