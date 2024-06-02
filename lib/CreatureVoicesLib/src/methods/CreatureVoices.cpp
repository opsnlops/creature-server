
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>


#include "model/HttpMethod.h"
#include "VoiceResult.h"
#include "CreatureVoices.h"

using json = nlohmann::json;

namespace creatures::voice {


    CreatureVoices::CreatureVoices(std::string apiKey) : apiKey(std::move(apiKey)) {}


}
