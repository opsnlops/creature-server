
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include <model/CreatureSpeechRequest.h>
#include <model/CreatureSpeechResponse.h>
#include <model/Voice.h>

#include "server/database.h"

#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/MakeSoundFileRequestDto.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/VoiceService.h"

#include "server/metrics/counters.h"

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures ::ws {

class VoiceController : public oatpp::web::server::api::ApiController {
  public:
    VoiceController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    VoiceService m_voiceService; // Create the sound service
  public:
    static std::shared_ptr<VoiceController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<VoiceController>(objectMapper);
    }

    ENDPOINT_INFO(getAllVoices) {
        info->summary = "Lists all of the voices files";
        info->addTag("Voice");

        info->addResponse<Object<VoiceListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/voice/list-available", getAllVoices, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/voice/list-available", "GET", "api/v1/voice/list-available", "getAllVoices",
                           "VoiceController", request, [&](const auto &span) {
                               const auto result = m_voiceService.getAllVoices();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(getSubscriptionStatus) {
        info->summary = "Returns the status of our subscription to the voice API";
        info->addTag("Voice");

        info->addResponse<Object<creatures::voice::SubscriptionDto>>(Status::CODE_200,
                                                                     "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/voice/subscription", getSubscriptionStatus,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/voice/subscription", "GET", "api/v1/voice/subscription",
                           "getSubscriptionStatus", "VoiceController", request, [&](const auto &span) {
                               const auto result = m_voiceService.getSubscriptionStatus();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(makeSoundFile) {
        info->summary = "Create a sound file for a creature based on the text given";
        info->addTag("Voice");

        info->addResponse<Object<creatures::voice::CreatureSpeechResponseDto>>(Status::CODE_200,
                                                                               "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/voice", makeSoundFile,
             BODY_DTO(Object<creatures::ws::MakeSoundFileRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/voice", "POST", "api/v1/voice", "makeSoundFile", "VoiceController", request,
                           [&](const auto &span) {
                               if (span && requestBody) {
                                   if (requestBody->creature_id) {
                                       span->setAttribute("creature.id", std::string(requestBody->creature_id));
                                   }
                                   if (requestBody->text) {
                                       const auto text = std::string(requestBody->text);
                                       span->setAttribute("speech.text_length", static_cast<int64_t>(text.size()));
                                       span->setAttribute("speech.text_preview", text.substr(0, 60));
                                   }
                               }

                               auto response = m_voiceService.generateCreatureSpeech(requestBody);

                               // Schedule an event to invalidate the sound list cache on the clients
                               scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::SoundList);

                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, response);
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen