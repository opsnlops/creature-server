

#include <vector>
#include <string>

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
    AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &metadataDto) {
        AnimationMetadata metadata;
        metadata.animation_id = metadataDto->animation_id;
        metadata.title = metadataDto->title;
        metadata.milliseconds_per_frame = metadataDto->milliseconds_per_frame;
        metadata.note = metadataDto->note;
        metadata.sound_file = metadataDto->sound_file;
        metadata.number_of_frames = metadataDto->number_of_frames;
        metadata.multitrack_audio = metadataDto->multitrack_audio;


        return metadata;
    }


    std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &metadata) {
        auto metadataDto = AnimationMetadataDto::createShared();
        metadataDto->animation_id = metadata.animation_id;
        metadataDto->title = metadata.title;
        metadataDto->milliseconds_per_frame = metadata.milliseconds_per_frame;
        metadataDto->note = metadata.note;
        metadataDto->sound_file = metadata.sound_file;
        metadataDto->number_of_frames = metadata.number_of_frames;
        metadataDto->multitrack_audio = metadata.multitrack_audio;

        return metadataDto.getPtr();
    }

}