
#include <string>
#include <vector>


#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Track.h"


namespace creatures {

    // Definitions of the static const arrays
    std::vector<std::string> animation_required_top_level_fields = {
            "id", "metadata", "tracks"
    };

    std::vector<std::string> animation_required_metadata_fields = {
            "animation_id", "title", "milliseconds_per_frame",
            "sound_file", "number_of_frames", "multitrack_audio"
    };

    std::vector<std::string> animation_required_track_fields = {
            "id", "creature_id", "animation_id", "frames"
    };


    std::shared_ptr<AnimationDto> convertToDto(const Animation &animation) {
        auto animationDto = AnimationDto::createShared();
        animationDto->id = animation.id;
        animationDto->metadata = convertToDto(animation.metadata);
        animationDto->tracks = oatpp::Vector<oatpp::Object<TrackDto>>::createShared();

        for (const auto &frame : animation.tracks) {
            animationDto->tracks->emplace_back(convertToDto(frame));
        }

        return animationDto.getPtr();
    }

    Animation convertFromDto(const std::shared_ptr<AnimationDto> &animationDto) {
        Animation animation;
        animation.id = animationDto->id;
        animation.metadata = convertFromDto(animationDto->metadata.getPtr());


        for (const auto &frameDto : *animationDto->tracks.getPtr()) {
            animation.tracks.push_back(convertFromDto(frameDto.getPtr()));
        }

        return animation;
    }

}