#pragma once

#include <fmt/format.h>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/namespace-stuffs.h"
#include "server/voice/WhisperLipSyncProcessor.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/SpeechToTextDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/ObservabilityManager.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::ws {

class SpeechToTextController : public oatpp::web::server::api::ApiController {
  public:
    SpeechToTextController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  public:
    static std::shared_ptr<SpeechToTextController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<SpeechToTextController>(objectMapper);
    }

    // --- Transcribe audio ---

    ENDPOINT_INFO(transcribeAudio) {
        info->summary = "Transcribe 16kHz mono float32 PCM audio to text";
        info->description =
            "Accepts raw 16kHz mono float32 PCM audio as the request body. "
            "Returns the transcribed text. Used by creature-listener to offload "
            "STT from the Pi to the server's faster CPU.";
        info->addTag("Speech-to-Text");
        info->addResponse<Object<SpeechToTextResponseDto>>(Status::CODE_200,
                                                            "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/stt/transcribe", transcribeAudio,
             BODY_STRING(String, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {

        auto span = creatures::observability
                        ? creatures::observability->createRequestSpan(
                              "POST /api/v1/stt/transcribe", "POST",
                              "api/v1/stt/transcribe", extractTraceparent(request))
                        : nullptr;
        addHttpRequestAttributes(span, request);

        auto startTime = std::chrono::steady_clock::now();

        if (!body || body->empty()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "Request body is empty — send raw 16kHz mono float32 PCM audio";
            if (span) { span->setError("Empty body"); span->setHttpStatus(400); }
            return createDtoResponse(Status::CODE_400, result);
        }

        // Interpret the body as raw float32 samples
        auto bodyData = body->data();
        auto bodySize = body->size();

        if (bodySize % sizeof(float) != 0) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "Body size is not a multiple of 4 bytes (expected float32 PCM)";
            if (span) { span->setError("Invalid body size"); span->setHttpStatus(400); }
            return createDtoResponse(Status::CODE_400, result);
        }

        size_t numSamples = bodySize / sizeof(float);
        const auto* floatData = reinterpret_cast<const float*>(bodyData);
        std::vector<float> audioData(floatData, floatData + numSamples);

        float durationSec = static_cast<float>(numSamples) / 16000.0f;
        info("STT request: {:.1f}s of audio ({} samples, {} bytes)",
             durationSec, numSamples, bodySize);

        if (span) {
            span->setAttribute("audio.duration_sec", fmt::format("{:.1f}", durationSec));
            span->setAttribute("audio.samples", static_cast<int64_t>(numSamples));
        }

        // Transcribe using the shared whisper context
        auto &processor = creatures::voice::WhisperLipSyncProcessor::instance();
        auto operationSpan = (span && creatures::observability)
            ? creatures::observability->createOperationSpan("stt.transcribe", span)
            : nullptr;
        auto transcribeResult = processor.transcribe(audioData, operationSpan);

        auto endTime = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        if (!transcribeResult.isSuccess()) {
            auto result = StatusDto::createShared();
            result->status = "error";
            result->code = 500;
            result->message = transcribeResult.getError()->getMessage().c_str();
            if (span) { span->setError(transcribeResult.getError()->getMessage()); span->setHttpStatus(500); }
            return createDtoResponse(Status::CODE_500, result);
        }

        auto transcript = transcribeResult.getValue().value();

        auto response = SpeechToTextResponseDto::createShared();
        response->status = "ok";
        response->transcript = transcript.c_str();
        response->audio_duration_sec = static_cast<double>(durationSec);
        response->transcription_time_ms = elapsedMs;

        if (span) {
            span->setAttribute("transcript.length", static_cast<int64_t>(transcript.size()));
            span->setAttribute("transcription.time_ms", fmt::format("{:.0f}", elapsedMs));
            span->setHttpStatus(200);
        }

        info("STT complete in {:.0f}ms: \"{}\"", elapsedMs, transcript);

        return createDtoResponse(Status::CODE_200, response);
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
