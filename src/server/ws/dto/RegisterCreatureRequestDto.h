
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * @brief DTO for registering a creature with its universe assignment
 *
 * This is used when a controller starts up and registers a creature, providing both the
 * creature's configuration (from its local JSON file) and the universe it's currently assigned to.
 */
class RegisterCreatureRequestDto : public oatpp::DTO {

    DTO_INIT(RegisterCreatureRequestDto, DTO)

    DTO_FIELD_INFO(creature_config) {
        info->description = "The creature's JSON configuration (source of truth from the controller's config file)";
        info->required = true;
    }
    DTO_FIELD(String, creature_config);

    DTO_FIELD_INFO(universe) {
        info->description = "The universe this creature is currently assigned to on its controller";
        info->required = true;
    }
    DTO_FIELD(UInt32, universe);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
