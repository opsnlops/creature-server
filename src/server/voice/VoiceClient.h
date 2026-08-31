#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "util/Result.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::voice {

struct CreatureSpeechRequest {
    std::string creature_name;
    std::string title;
    std::string voice_id;
    std::string model_id;
    float stability{0.0F};
    float similarity_boost{0.0F};
    std::string text;
};

struct CreatureSpeechResponse {
    bool success{false};
    std::string sound_file_name;
    std::string transcript_file_name;
    uint32_t sound_file_size{0};
};

struct Subscription {
    std::string tier;
    std::string status;
    uint32_t character_count{0};
    uint32_t character_limit{0};
};

struct Voice {
    std::string voiceId;
    std::string name;
};

class VoiceClient {
  public:
    explicit VoiceClient(std::string apiKey);

    Result<std::vector<Voice>> listAllAvailableVoices(std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<Subscription> getSubscriptionStatus(std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<CreatureSpeechResponse> generateCreatureSpeech(const std::filesystem::path &fileSavePath,
                                                          const CreatureSpeechRequest &speechRequest,
                                                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

  private:
    std::string apiKey_;

    static std::string makeFileName(const CreatureSpeechRequest &speechRequest);
    static std::string toLowerAndReplaceSpaces(std::string value);
};

} // namespace creatures::voice
