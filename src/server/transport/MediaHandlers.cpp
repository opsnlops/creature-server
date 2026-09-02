#include "server/transport/MediaHandlers.h"

#include <memory>

#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "api/SoundRequests.h"
#include "api/VoiceContracts.h"
#include "model/Sound.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/ws/service/SoundService.h"
#include "server/ws/service/VoiceService.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

namespace creatures::transport {
namespace {
PreparedResponse failure(const ServerError &error, const std::shared_ptr<OperationSpan> &span) {
    const auto code = serverErrorToStatusCode(error.getCode());
    recordSpanError(span, error.getMessage(), "MediaOperationFailed", error.getCode());
    return PreparedResponse::json(
        code, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(code, error.getMessage()))));
}
} // namespace

PreparedResponse listSounds(const bool adHoc, const std::shared_ptr<OperationSpan> &span) {
    ws::SoundService service;
    if (adHoc) {
        const auto result = service.getAdHocSounds();
        if (!result.isSuccess())
            return failure(result.getError().value(), span);
        if (span)
            span->setSuccess();
        return PreparedResponse::json(
            200, api::jsonToString(api::listResponseToJson(result.getValue().value(), api::adHocSoundEntryToJson)));
    }
    const auto result = service.getAllSounds();
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200,
                                  api::jsonToString(api::listResponseToJson(result.getValue().value(), soundToJson)));
}

PreparedResponse playSound(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto json = JsonParser::parseApiJsonString(body, "sound play request", span);
    if (!json.isSuccess())
        return failure(json.getError().value(), span);
    const auto parsed = api::playSoundRequestFromJson(json.getValue().value());
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    ws::SoundService service;
    const auto result = service.playSound(parsed.getValue()->fileName);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::statusResponseToJson(result.getValue().value())));
}

PreparedResponse listVoices(const std::shared_ptr<OperationSpan> &span) {
    ws::VoiceService service;
    const auto result = service.getAllVoices();
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(
        200, api::jsonToString(api::listResponseToJson(result.getValue().value(), api::voiceToJson)));
}

PreparedResponse voiceSubscription(const std::shared_ptr<OperationSpan> &span) {
    ws::VoiceService service;
    const auto result = service.getSubscriptionStatus();
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::subscriptionToJson(result.getValue().value())));
}

PreparedResponse createVoiceFile(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto json = JsonParser::parseApiJsonString(body, "voice file request", span);
    if (!json.isSuccess())
        return failure(json.getError().value(), span);
    const auto parsed = api::makeSoundFileRequestFromJson(json.getValue().value());
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    const auto jobId =
        creatures::jobManager->createJob(jobs::JobType::VoiceFile, api::makeSoundFileRequestToJson(request).dump());
    creatures::jobWorker->queueJob(jobId);
    if (span)
        span->setSuccess();
    const api::JobCreatedResponse response{
        jobId, "voice-file",
        "Voice file job created. Listen for job-progress and job-complete WebSocket messages on this job_id, or "
        "poll GET /api/v1/job/{job_id}."};
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}
} // namespace creatures::transport
