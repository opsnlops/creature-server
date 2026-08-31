#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "api/VoiceContracts.h"
#include "server/voice/VoiceClient.h"
#include "util/Result.h"

namespace creatures {
class OperationSpan;
class RequestSpan;
} // namespace creatures

namespace creatures::ws {

class VoiceService {
  public:
    explicit VoiceService(std::shared_ptr<voice::VoiceClient> voiceClient = nullptr)
        : voiceClient_(std::move(voiceClient)) {}

    Result<std::vector<voice::Voice>> getAllVoices(std::shared_ptr<RequestSpan> parentSpan = nullptr) const;
    Result<voice::Subscription> getSubscriptionStatus(std::shared_ptr<RequestSpan> parentSpan = nullptr) const;
    Result<voice::CreatureSpeechResponse>
    generateCreatureSpeech(const api::MakeSoundFileRequest &request,
                           std::shared_ptr<OperationSpan> parentSpan = nullptr) const;

  private:
    Result<std::shared_ptr<voice::VoiceClient>> resolveClient() const;

    std::shared_ptr<voice::VoiceClient> voiceClient_;
};

} // namespace creatures::ws
