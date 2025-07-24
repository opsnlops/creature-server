

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "AnimationMetadata.h"

namespace creatures {

/*
 *         DTO_FIELD(String, animation_id);
        DTO_FIELD(String, title);
        DTO_FIELD(UInt32, milliseconds_per_frame);
        DTO_FIELD(String, note);
        DTO_FIELD(String, sound_file);
        DTO_FIELD(UInt32, number_of_frames);
        DTO_FIELD(Boolean, multitrack_audio);
 */

// Convert a CreatureDto to a Creature
AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &animationMetadataDto) {
    AnimationMetadata metadata;
    metadata.animation_id = animationMetadataDto->animation_id;
    metadata.title = animationMetadataDto->title;
    metadata.milliseconds_per_frame = animationMetadataDto->milliseconds_per_frame;
    metadata.note = animationMetadataDto->note;
    metadata.sound_file = animationMetadataDto->sound_file;
    metadata.number_of_frames = animationMetadataDto->number_of_frames;
    metadata.multitrack_audio = animationMetadataDto->multitrack_audio;

    return metadata;
}

std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &animationMetadata) {
    auto metadataDto = AnimationMetadataDto::createShared();
    metadataDto->animation_id = animationMetadata.animation_id;
    metadataDto->title = animationMetadata.title;
    metadataDto->milliseconds_per_frame = animationMetadata.milliseconds_per_frame;
    metadataDto->note = animationMetadata.note;
    metadataDto->sound_file = animationMetadata.sound_file;
    metadataDto->number_of_frames = animationMetadata.number_of_frames;
    metadataDto->multitrack_audio = animationMetadata.multitrack_audio;

    return metadataDto.getPtr();
}

} // namespace creatures