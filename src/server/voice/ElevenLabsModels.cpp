#include <memory>
#include <string>

#include "server/config/Configuration.h"
#include "server/voice/ElevenLabsModels.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
}

namespace creatures::voice {

std::string expressiveModelId() {
    // No Configuration means the built-in default rather than a crash: the test
    // binary leaves the global null, and computeCacheKey() reaches for the model
    // from a couple of dozen tests (issue #244).
    return config ? config->getExpressiveVoiceModel() : std::string(kDefaultExpressiveModelId);
}

} // namespace creatures::voice
