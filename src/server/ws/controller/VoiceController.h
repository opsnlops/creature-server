
#pragma once

#include <fmt/format.h>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/JsonResponse.h"
#include "api/VoiceContracts.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/storage/Storage.h"

#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/MakeSoundFileRequestDto.h"
#include "server/ws/dto/StatusDto.h"
#include "server/ws/service/VoiceService.h"

#include "server/metrics/counters.h"

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures {
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

namespace creatures ::ws {

class VoiceController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<VoiceController> {
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

        info->addResponse<String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/voice/list-available", getAllVoices, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/voice/list-available", "GET", "api/v1/voice/list-available", "getAllVoices",
                           "VoiceController", request, [&](const auto &span) {
                               const auto result = m_voiceService.getAllVoices(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(
                                   span, Status::CODE_200,
                                   api::listResponseToJson(result.getValue().value(), api::voiceToJson));
                           });
    }

    ENDPOINT_INFO(getSubscriptionStatus) {
        info->summary = "Returns the status of our subscription to the voice API";
        info->addTag("Voice");

        info->addResponse<String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/voice/subscription", getSubscriptionStatus,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/voice/subscription", "GET", "api/v1/voice/subscription",
                           "getSubscriptionStatus", "VoiceController", request, [&](const auto &span) {
                               const auto result = m_voiceService.getSubscriptionStatus(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   api::subscriptionToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(makeSoundFile) {
        info->summary = "Create a sound file for a creature based on the text given (async job)";
        info->description = "Single-voice TTS of the given text. Long text can outlive a 60s HTTP timeout, so this "
                            "returns 202 with a job_id; the worker generates the sound file asynchronously and "
                            "publishes progress + completion over the WebSocket job-progress stream. The completion "
                            "result is the CreatureSpeechResponse JSON the sync path used to return.";
        info->addTag("Voice");

        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/voice", makeSoundFile,
             BODY_DTO(Object<creatures::ws::MakeSoundFileRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/voice", "POST", "api/v1/voice", "makeSoundFile", "VoiceController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!requestBody || !requestBody->creature_id || requestBody->creature_id->empty() ||
                                   !requestBody->text || requestBody->text->empty()) {
                                   return bailHttp(span, Status::CODE_400, "creature_id and text are required");
                               }
                               if (span) {
                                   span->setAttribute("creature.id", std::string(requestBody->creature_id));
                                   const auto text = std::string(requestBody->text);
                                   span->setAttribute("speech.text_length", static_cast<int64_t>(text.size()));
                               }

                               nlohmann::json details = {{"creature_id", std::string(*requestBody->creature_id)},
                                                         {"text", std::string(*requestBody->text)}};
                               if (requestBody->title)
                                   details["title"] = std::string(*requestBody->title);
                               auto parsed = api::makeSoundFileRequestFromJson(details);
                               if (!parsed.isSuccess())
                                   return bailFromServerError(span, parsed.getError().value());
                               const std::string detailsStr = details.dump();

                               const std::string jobId = creatures::jobManager->createJob(
                                   creatures::jobs::JobType::VoiceFile, detailsStr, span);
                               creatures::jobWorker->queueJob(jobId);
                               if (span) {
                                   span->setAttribute("job.id", jobId);
                                   span->setHttpStatus(202);
                               }
                               auto response = JobCreatedDto::createShared();
                               response->job_id = jobId.c_str();
                               response->job_type = "voice-file";
                               response->message =
                                   "Voice file job created. Listen for job-progress and job-complete WebSocket "
                                   "messages on this job_id, or poll GET /api/v1/job/{job_id}.";
                               return createDtoResponse(Status::CODE_202, response);
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen
