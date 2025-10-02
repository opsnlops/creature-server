
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "CreatureVoices.h"
#include "VoiceResult.h"
#include "model/HttpMethod.h"

using json = nlohmann::json;

namespace creatures::voice {

CreatureVoices::CreatureVoices(std::string apiKey_) : apiKey(std::move(apiKey_)) {}

} // namespace creatures::voice
