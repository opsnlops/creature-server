
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>


#include "server/metrics/counters.h"
#include "server/ws/service/MetricsService.h"
#include "server/ws/dto/StatusDto.h"

#include "server/metrics/counters.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures :: ws {

    class MetricsController : public oatpp::web::server::api::ApiController {
    public:
        MetricsController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        MetricsService m_metricsService;
    public:

        static std::shared_ptr<MetricsController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper) // Inject objectMapper component here as default parameter
        ) {
            return std::make_shared<MetricsController>(objectMapper);
        }


        ENDPOINT_INFO(counters) {
            info->summary = "Gets all of the system counters";

            info->addResponse<Object<SystemCountersDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/metric/counters", counters)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_metricsService.getCounters());
        }

    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen