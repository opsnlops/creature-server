

#include <string>

#include "Sound.h"

namespace creatures {

    Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto) {
        Sound sound;
        sound.fileName = soundDto->file_name;
        sound.size = soundDto->size;

        return sound;
    }

    std::shared_ptr<SoundDto> convertSoundToDto(const Sound &sound) {
        auto soundDto = SoundDto::createShared();
        soundDto->file_name = sound.fileName;
        soundDto->size = sound.size;

        return soundDto.getPtr();
    }

}