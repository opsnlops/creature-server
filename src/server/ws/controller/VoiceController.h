
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>

#include <model/CreatureSpeechRequest.h>
#include <model/CreatureSpeechResponse.h>
#include <model/Voice.h>

#include "server/database.h"

#include "server/ws/dto/MakeSoundFileRequestDto.h"
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

            info->addResponse<Object<VoiceListDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/voice/list-available", getAllVoices)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_voiceService.getAllVoices());
        }

        ENDPOINT_INFO(getSubscriptionStatus) {
            info->summary = "Returns the status of our subscription to the voice API";

            info->addResponse<Object<creatures::voice::SubscriptionDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/voice/subscription", getSubscriptionStatus)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_voiceService.getSubscriptionStatus());
        }

        ENDPOINT_INFO(makeSoundFile) {
            info->summary = "Create a sound file for a creature based on the text given";

            info->addResponse<Object<creatures::voice::CreatureSpeechResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/voice", makeSoundFile,
                 BODY_DTO(Object<creatures::ws::MakeSoundFileRequestDto>, requestBody))
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_voiceService.generateCreatureSpeech(requestBody));
        }
    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen