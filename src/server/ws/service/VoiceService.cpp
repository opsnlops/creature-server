#include "VoiceService.h"

#include <filesystem>
#include <utility>

#include "server/config/Configuration.h"
#include "server/voice/SpeechGenerationManager.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::ws {

Result<std::shared_ptr<voice::VoiceClient>> VoiceService::resolveClient() const {
    if (voiceClient_)
        return Result<std::shared_ptr<voice::VoiceClient>>{voiceClient_};
    if (!creatures::config)
        return Result<std::shared_ptr<voice::VoiceClient>>{
            ServerError(ServerError::InternalError, "Voice configuration unavailable")};
    return Result<std::shared_ptr<voice::VoiceClient>>{
        std::make_shared<voice::VoiceClient>(creatures::config->getVoiceApiKey())};
}

Result<std::vector<voice::Voice>> VoiceService::getAllVoices(std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("VoiceService.getAllVoices", std::move(parentSpan))
                    : nullptr;
    auto client = resolveClient();
    if (!client.isSuccess()) {
        recordSpanError(span, client.getError().value().getMessage(), "DependencyUnavailable",
                        client.getError().value().getCode());
        return Result<std::vector<voice::Voice>>{client.getError().value()};
    }
    auto result = client.getValue().value()->listAllAvailableVoices(span);
    if (!result.isSuccess()) {
        auto error = result.getError().value();
        recordSpanError(span, error.getMessage(), "VoiceApiError", error.getCode());
        return Result<std::vector<voice::Voice>>{std::move(error)};
    }
    auto voices = result.getValue().value();
    if (span) {
        span->setAttribute("voice.count", static_cast<int64_t>(voices.size()));
        span->setSuccess();
    }
    return Result<std::vector<voice::Voice>>{std::move(voices)};
}

Result<voice::Subscription> VoiceService::getSubscriptionStatus(std::shared_ptr<RequestSpan> parentSpan) const {
    auto span =
        creatures::observability
            ? creatures::observability->createOperationSpan("VoiceService.getSubscriptionStatus", std::move(parentSpan))
            : nullptr;
    auto client = resolveClient();
    if (!client.isSuccess()) {
        recordSpanError(span, client.getError().value().getMessage(), "DependencyUnavailable",
                        client.getError().value().getCode());
        return Result<voice::Subscription>{client.getError().value()};
    }
    auto result = client.getValue().value()->getSubscriptionStatus(span);
    if (!result.isSuccess()) {
        auto error = result.getError().value();
        recordSpanError(span, error.getMessage(), "VoiceApiError", error.getCode());
        return Result<voice::Subscription>{std::move(error)};
    }
    auto subscription = result.getValue().value();
    if (span) {
        span->setAttribute("voice.subscription.tier", subscription.tier);
        span->setAttribute("voice.subscription.status", subscription.status);
        span->setAttribute("voice.subscription.character_count", static_cast<int64_t>(subscription.character_count));
        span->setAttribute("voice.subscription.character_limit", static_cast<int64_t>(subscription.character_limit));
        span->setAttribute("voice.subscription.characters_remaining",
                           static_cast<int64_t>(subscription.character_limit) - subscription.character_count);
        span->setSuccess();
    }
    return Result<voice::Subscription>{std::move(subscription)};
}

Result<voice::CreatureSpeechResponse>
VoiceService::generateCreatureSpeech(const api::MakeSoundFileRequest &request,
                                     std::shared_ptr<OperationSpan> parentSpan) const {
    auto speechSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                     "VoiceService.generateCreatureSpeech", std::move(parentSpan))
                                               : nullptr;
    if (speechSpan) {
        speechSpan->setAttribute("creature.id", request.creatureId);
        speechSpan->setAttribute("speech.text_length", static_cast<int64_t>(request.text.size()));
    }
    auto client = resolveClient();
    if (!client.isSuccess()) {
        recordSpanError(speechSpan, client.getError().value().getMessage(), "DependencyUnavailable",
                        client.getError().value().getCode());
        return Result<voice::CreatureSpeechResponse>{client.getError().value()};
    }

    voice::SpeechGenerationRequest helperRequest;
    helperRequest.creatureId = request.creatureId;
    helperRequest.text = request.text;
    helperRequest.title = request.title.value_or("");
    helperRequest.outputDirectory = std::filesystem::path(creatures::config->getSoundFileLocation());
    helperRequest.parentSpan = speechSpan;
    helperRequest.voiceClient = client.getValue().value();

    auto result = voice::SpeechGenerationManager::generate(helperRequest);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        recordSpanError(speechSpan, error.getMessage(), "SpeechGenerationError", error.getCode());
        return Result<voice::CreatureSpeechResponse>{error};
    }
    if (speechSpan)
        speechSpan->setSuccess();
    return Result<voice::CreatureSpeechResponse>{result.getValue().value().response};
}

} // namespace creatures::ws
