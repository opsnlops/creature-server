
#pragma once

#include <fmt/format.h>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "api/VoiceContracts.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/storage/Storage.h"

#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
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
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
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
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
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

        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/voice", makeSoundFile, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/voice", "POST", "api/v1/voice", "makeSoundFile", "VoiceController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_VOICE_REQUEST_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "VoiceController.parseMakeSoundFileRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "voice.file.create");
                const auto json = JsonParser::parseApiJsonString(body, "voice file request", parseSpan);
                if (!json.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, json.getError().value());
                }
                const auto parsed = api::makeSoundFileRequestFromJson(json.getValue().value());
                if (!parsed.isSuccess()) {
                    const auto error = parsed.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidVoiceFileRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto voiceRequest = parsed.getValue().value();
                if (span) {
                    span->setAttribute("creature.id", canonicalUuid(voiceRequest.creatureId));
                    span->setAttribute("speech.text_length", static_cast<int64_t>(voiceRequest.text.size()));
                    span->setAttribute("speech.has_title", voiceRequest.title.has_value());
                }

                const auto details = api::makeSoundFileRequestToJson(voiceRequest);
                const std::string detailsStr = details.dump();

                const std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::VoiceFile, detailsStr, span);
                creatures::jobWorker->queueJob(jobId);
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                const api::JobCreatedResponse response{
                    jobId, "voice-file",
                    "Voice file job created. Listen for job-progress and job-complete WebSocket "
                    "messages on this job_id, or poll GET /api/v1/job/{job_id}."};
                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen
