
#pragma once

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/AnimationMetadata.h"
#include "model/Track.h"

namespace creatures {

    struct Animation {
        std::string id;
        AnimationMetadata metadata;
        std::vector<Track> tracks;

        // List of required fields
        static constexpr std::array<const char*, 3> required_top_level_fields =
                {"id", "metadata", "tracks"};

        static constexpr std::array<const char*, 6> required_metadata_fields =
                {"animation_id", "title", "milliseconds_per_frame",
                 "sound_file", "number_of_frames", "multitrack_audio"};

        static constexpr std::array<const char*, 4> required_track_fields =
                {"id", "creature_id", "animation_id", "frames"};

    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class AnimationDto : public oatpp::DTO {

        DTO_INIT(AnimationDto, DTO /* extends */);

        DTO_FIELD_INFO(id) {
            info->description = "Animation ID in the form of a UUID";
        }
        DTO_FIELD(String, id);

        DTO_FIELD_INFO(metadata) {
            info->description = "An AnimationMetadataDto with the data for this animation";
        }
        DTO_FIELD(Object<AnimationMetadataDto>, metadata);

        DTO_FIELD_INFO(tracks) {
            info->description = "The tracks of motion data";
        }
        DTO_FIELD(Vector<oatpp::Object<TrackDto>>, tracks);

    };

#include OATPP_CODEGEN_END(DTO)

    std::shared_ptr<AnimationDto> convertToDto(const Animation &creature);
    Animation convertFromDto(const std::shared_ptr<AnimationDto> &creatureDto);


}

