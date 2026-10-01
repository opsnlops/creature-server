#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "model/Creature.h"
#include "server/voice/VoiceClient.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures {
class Database;
class Configuration;
class ObservabilityManager;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::voice {

struct SpeechGenerationRequest {
    std::string creatureId;
    std::string text;
    std::string title;
    std::filesystem::path outputDirectory;
    std::shared_ptr<OperationSpan> parentSpan{nullptr};
    std::shared_ptr<VoiceClient> voiceClient{nullptr};
    /// Render with this model instead of the creature's voice.model_id. The
    /// creature's model is the fast one tuned for ad-hoc speech; pre-rendered
    /// voice files ask for the expressive model so audio tags are performed.
    std::optional<std::string> modelIdOverride;
};

struct SpeechGenerationResult {
    creatures::voice::CreatureSpeechResponse response;
    std::filesystem::path mp3Path;
    std::filesystem::path wavPath;
    std::filesystem::path transcriptPath;
    uint16_t audioChannel{1};
    creatures::Creature creature;
    nlohmann::json creatureJson;
};

class SpeechGenerationManager {
  public:
    static Result<SpeechGenerationResult> generate(const SpeechGenerationRequest &request);
};

} // namespace creatures::voice
