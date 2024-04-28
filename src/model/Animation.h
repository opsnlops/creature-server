
#pragma once

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>



#include "model/AnimationMetadata.h"
#include "model/FrameData.h"

namespace creatures {

    struct Animation {
        std::string id;
        AnimationMetadata metadata;
        std::vector<FrameData> tracks;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class AnimationDTO : public oatpp::DTO {

        DTO_INIT(AnimationDTO, DTO /* extends */);

        DTO_FIELD(String, id);
        DTO_FIELD(Int32, age);

    };

#include OATPP_CODEGEN_END(DTO)


}

