#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/DmxFixture.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)
class FixtureChannelDto : public oatpp::DTO {
    DTO_INIT(FixtureChannelDto, DTO)
    DTO_FIELD(UInt16, offset);
    DTO_FIELD(String, name);
    DTO_FIELD(String, kind);
};
class FixturePatternValueDto : public oatpp::DTO {
    DTO_INIT(FixturePatternValueDto, DTO)
    DTO_FIELD(String, channel);
    DTO_FIELD(UInt8, value);
};
class FixturePatternDto : public oatpp::DTO {
    DTO_INIT(FixturePatternDto, DTO)
    DTO_FIELD(String, id);
    DTO_FIELD(String, name);
    DTO_FIELD(List<Object<FixturePatternValueDto>>, values);
    DTO_FIELD(UInt32, fade_in_ms);
    DTO_FIELD(UInt32, fade_out_ms);
    DTO_FIELD(UInt32, hold_ms);
};
class FixtureBindingDto : public oatpp::DTO {
    DTO_INIT(FixtureBindingDto, DTO)
    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, on_reason);
    DTO_FIELD(String, on_state);
    DTO_FIELD(String, pattern_id);
};
class DmxFixtureDto : public oatpp::DTO {
    DTO_INIT(DmxFixtureDto, DTO)
    DTO_FIELD(String, id);
    DTO_FIELD(String, name);
    DTO_FIELD(String, type);
    DTO_FIELD(UInt16, channel_offset);
    DTO_FIELD(UInt32, assigned_universe);
    DTO_FIELD(List<Object<FixtureChannelDto>>, channels);
    DTO_FIELD(List<Object<FixturePatternDto>>, patterns);
    DTO_FIELD(List<Object<FixtureBindingDto>>, bindings);
};
#include OATPP_CODEGEN_END(DTO)

oatpp::Object<DmxFixtureDto> convertToDto(const DmxFixture &fixture);
DmxFixture convertFromDto(const std::shared_ptr<DmxFixtureDto> &fixtureDto);

} // namespace creatures
