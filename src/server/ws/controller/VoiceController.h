
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>


#include <model/Voice.h>

#include "server/database.h"


#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/VoiceService.h"

#include "server/metrics/counters.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures :: ws {

    class VoiceController : public oatpp::web::server::api::ApiController {
    public:
        VoiceController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        VoiceService m_voiceService; // Create the sound service
    public:

        static std::shared_ptr<VoiceController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper)
        ) {
            return std::make_shared<VoiceController>(objectMapper);
        }


        ENDPOINT_INFO(getAllVoices) {
            info->summary = "Lists all of the voices files";

            info->addResponse<Object<SoundsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/voice", getAllVoices)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_voiceService.getAllVoices());
        }




    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen