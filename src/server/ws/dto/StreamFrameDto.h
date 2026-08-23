#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/StreamFrame.h"
#include "util/ObservabilityManager.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class StreamFrameDto : public oatpp::DTO {
    DTO_INIT(StreamFrameDto, DTO)

    DTO_FIELD_INFO(creature_id) { info->description = "The creature receiving the streamed frame"; }
    DTO_FIELD(String, creature_id);
    DTO_FIELD_INFO(universe) { info->description = "The E1.31 universe"; }
    DTO_FIELD(UInt32, universe);
    DTO_FIELD_INFO(data) { info->description = "Base64-encoded joint positions"; }
    DTO_FIELD(String, data);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<StreamFrameDto> convertToDto(const StreamFrame &streamFrame,
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);
StreamFrame convertFromDto(const oatpp::Object<StreamFrameDto> &streamFrameDto,
                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

} // namespace creatures
