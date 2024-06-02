
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


    VoiceResult<std::vector<Voice>> CreatureVoices::listAllAvailableVoices() {
        const std::string url ="https://api.elevenlabs.io/v1/voices";

        debug("Fetching available voices");

        auto curlHandle = createCurlHandle(url);
        auto res = performRequest(curlHandle, apiKey, HttpMethod::GET, "");
        if(!res.isSuccess()) {
            auto error = res.getError();
            std::string errorMessage = fmt::format("Failed to fetch available voices: {}", error->getMessage());
            return VoiceResult<std::vector<Voice>>{VoiceError(error->getCode(), error->getMessage())};
        }

        auto httpResponse = res.getValue().value();
        trace("httpResponse was: {}", httpResponse);


        json jsonResponse;
        try {
            jsonResponse = json::parse(httpResponse);
        } catch (const json::parse_error& e) {
            std::string errorMessage = fmt::format("Failed to parse JSON response: {}", e.what());
            warn(errorMessage);
            return VoiceResult<std::vector<Voice>>{VoiceError(VoiceError::InvalidData, errorMessage)};
        }

        std::vector<Voice> voices;
        for (const auto& item : jsonResponse["voices"]) {
            Voice voice;
            voice.voiceId = item["voice_id"].get<std::string>();
            voice.name = item["name"].get<std::string>();
            voices.push_back(voice);
        }
        debug("ElevenLabs gave us {} voices", voices.size());


        // Sort the voices by name
        std::sort(voices.begin(), voices.end(), [](const Voice& a, const Voice& b) {
            return a.name < b.name;
        });

        return voices;
    }

}
