
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>


#include "server/database.h"

#include "server/ws/service/CreatureService.h"
#include "server/metrics/counters.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures :: ws {

    class CreatureController : public oatpp::web::server::api::ApiController {
    public:
        CreatureController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        CreatureService m_creatureService; // Create the creature service
    public:

        static std::shared_ptr<CreatureController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper) // Inject objectMapper component here as default parameter
        ) {
            return std::make_shared<CreatureController>(objectMapper);
        }

        ENDPOINT_INFO(getAllCreatures) {
            info->summary = "Get all of the creatures";

            info->addResponse<Object<CreaturesListDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/creature", getAllCreatures)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_creatureService.getAllCreatures());
        }


        ENDPOINT_INFO(getCreature) {
            info->summary = "Get one creature by id";

            info->addResponse<Object<CreatureDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");

            info->pathParams["creatureId"].description = "Creature ID in the form of a MongoDB OID";
        }
        ENDPOINT("GET", "api/v1/creature/{creatureId}", getCreature,
                 PATH(String, creatureId))
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_creatureService.getCreature(creatureId));
        }
    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen