

#include <string>

#include "Sound.h"

namespace creatures {

Sound convertSoundFromDto(const std::shared_ptr<SoundDto> &soundDto) {
    Sound sound;
    sound.fileName = soundDto->file_name;
    sound.size = soundDto->size;
    sound.transcript = soundDto->transcript;
    sound.lipsync = soundDto->lipsync;
    sound.title = soundDto->title ? std::string(soundDto->title) : std::string();
    sound.sourceScriptId = soundDto->source_script_id ? std::string(soundDto->source_script_id) : std::string();
    sound.script = soundDto->script ? std::string(soundDto->script) : std::string();
    sound.generationIds = soundDto->generation_ids ? std::string(soundDto->generation_ids) : std::string();
    sound.hasEmbeddedScript = soundDto->has_embedded_script.getValue(false);
    sound.hasEmbeddedLipsync = soundDto->has_embedded_lipsync.getValue(false);

    return sound;
}

oatpp::Object<SoundDto> convertSoundToDto(const Sound &sound) {
    auto soundDto = SoundDto::createShared();
    soundDto->file_name = sound.fileName;
    soundDto->size = sound.size;
    soundDto->transcript = sound.transcript;
    soundDto->lipsync = sound.lipsync;
    soundDto->title = sound.title;
    soundDto->source_script_id = sound.sourceScriptId;
    soundDto->script = sound.script;
    soundDto->generation_ids = sound.generationIds;
    soundDto->has_embedded_script = sound.hasEmbeddedScript;
    soundDto->has_embedded_lipsync = sound.hasEmbeddedLipsync;

    return soundDto;
}

} // namespace creatures