
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * @brief Request body for PUT /api/v1/fixture/{fixtureId}/universe
 *
 * Universe assignment is persisted on the fixture document and mirrored to
 * `fixtureUniverseMap` for fast runtime lookups.
 */
class SetFixtureUniverseRequestDto : public oatpp::DTO {

    DTO_INIT(SetFixtureUniverseRequestDto, DTO)

    DTO_FIELD_INFO(universe) {
        info->description = "The E1.31 universe number this fixture should output to.";
        info->required = true;
    }
    DTO_FIELD(UInt32, universe);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
