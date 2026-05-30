
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/DialogScript.h"

namespace creatures {

struct AnimationMetadata {
    std::string animation_id;
    std::string title;
    uint32_t milliseconds_per_frame;
    std::string note;
    std::string sound_file;
    uint32_t number_of_frames;
    bool multitrack_audio;

    // Soft pointer + copy-on-write snapshot for animations rendered from a
    // saved DialogScript. Both empty for non-dialog animations or for dialog
    // animations submitted with inline turns. `source_script_turns` is a
    // snapshot of the script's turns AT RENDER TIME — preserved so the
    // animation remains traceable even after the parent script is edited
    // or deleted.
    std::string source_script_id;
    std::vector<DialogScriptTurn> source_script_turns;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class AnimationMetadataDto final : public oatpp::DTO {

    DTO_INIT(AnimationMetadataDto, DTO /* extends */)

    DTO_FIELD_INFO(animation_id) { info->description = "Animation ID in the form of a MongoDB OID"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(title) { info->description = "The title for this animation"; }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(milliseconds_per_frame) { info->description = "The number of milliseconds per frame (usually 20)"; }
    DTO_FIELD(UInt32, milliseconds_per_frame);

    DTO_FIELD_INFO(note) { info->description = "Any notes to save in the database"; }
    DTO_FIELD(String, note);

    DTO_FIELD_INFO(sound_file) { info->description = "The sound file to play with this animation"; }
    DTO_FIELD(String, sound_file);

    DTO_FIELD_INFO(number_of_frames) { info->description = "The number of frames in the animation"; }
    DTO_FIELD(UInt32, number_of_frames);

    DTO_FIELD_INFO(multitrack_audio) { info->description = "True if the audio is multitrack"; }
    DTO_FIELD(Boolean, multitrack_audio);

    DTO_FIELD_INFO(source_script_id) {
        info->description = "If this animation was rendered from a saved DialogScript, the script's UUID. Empty/absent "
                            "otherwise. Soft pointer — the script may have been edited or deleted; "
                            "source_script_turns is the authoritative snapshot.";
        info->required = false;
    }
    DTO_FIELD(String, source_script_id);

    DTO_FIELD_INFO(source_script_turns) {
        info->description = "Copy-on-write snapshot of the script's turns at the moment this animation was rendered. "
                            "Absent for animations not rendered from a script.";
        info->required = false;
    }
    DTO_FIELD(List<Object<DialogScriptTurnDto>>, source_script_turns);
};

std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &animationMetadata);
AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &animationMetadataDto);

#include OATPP_CODEGEN_END(DTO)
} // namespace creatures
