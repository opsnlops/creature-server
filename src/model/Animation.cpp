

#include <vector>
#include <string>


#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/FrameData.h"


namespace creatures {

    std::shared_ptr<AnimationDto> convertToDto(const Animation &animation) {
        auto animationDto = AnimationDto::createShared();
        animationDto->id = animation.id;
        animationDto->metadata = convertToDto(animation.metadata);

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