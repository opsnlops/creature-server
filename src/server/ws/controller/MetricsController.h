
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/MetricsService.h"

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures ::ws {

class MetricsController : public oatpp::web::server::api::ApiController {
  public:
    MetricsController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    MetricsService m_metricsService;

  public:
    static std::shared_ptr<MetricsController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                 objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<MetricsController>(objectMapper);
    }

    ENDPOINT_INFO(counters) {
        info->summary = "Gets all of the system counters";
        info->addTag("Metrics");

        info->addResponse<Object<SystemCountersDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/metric/counters", counters, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/metric/counters", "GET", "api/v1/metric/counters", "counters",
                           "MetricsController", request, [&](const auto &span) {
                               const auto result = m_metricsService.getCounters();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen