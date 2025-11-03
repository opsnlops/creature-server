#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO) // NOLINT

namespace creatures::ws {

class RhubarbMetadataDto : public oatpp::DTO {
    DTO_INIT(RhubarbMetadataDto, DTO)

    DTO_FIELD(String, soundFile, "soundFile");
    DTO_FIELD(Float64, duration, "duration");
};

class RhubarbMouthCueDto : public oatpp::DTO {
    DTO_INIT(RhubarbMouthCueDto, DTO)

    DTO_FIELD(Float64, start, "start");
    DTO_FIELD(Float64, end, "end");
    DTO_FIELD(String, value, "value");
};

class GenerateLipSyncUploadResponseDto : public oatpp::DTO {
    DTO_INIT(GenerateLipSyncUploadResponseDto, DTO)

    DTO_FIELD(Object<RhubarbMetadataDto>, metadata, "metadata");
    DTO_FIELD(List<Object<RhubarbMouthCueDto>>, mouthCues, "mouthCues");
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO) // NOLINT
