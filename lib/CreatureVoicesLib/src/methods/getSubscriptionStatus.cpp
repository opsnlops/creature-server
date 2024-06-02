
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>


#include "model/HttpMethod.h"
#include "model/Subscription.h"
#include "VoiceResult.h"
#include "CreatureVoices.h"

using json = nlohmann::json;

namespace creatures::voice {

    VoiceResult<Subscription> CreatureVoices::getSubscriptionStatus() {
        const std::string url = "/v1/user/subscription";

        debug("Getting the current status of the subscription");

        auto curlHandle = createCurlHandle(url);
        curlHandle.addHeader("Content-Type: application/json");

        auto res = performRequest(curlHandle, apiKey, HttpMethod::GET, "");
        if(!res.isSuccess()) {
            auto error = res.getError();
            std::string errorMessage = fmt::format("Failed to our subscription status: {}", error->getMessage());
            return VoiceResult<Subscription>{VoiceError(error->getCode(), error->getMessage())};
        }

        auto httpResponse = res.getValue().value();
        trace("httpResponse was: {}", httpResponse);


        json jsonResponse;
        try {
            jsonResponse = json::parse(httpResponse);
        } catch (const json::parse_error& e) {
            std::string errorMessage = fmt::format("Failed to parse JSON response: {}", e.what());
            warn(errorMessage);
            return VoiceResult<Subscription>{VoiceError(VoiceError::InvalidData, errorMessage)};
        }

        // Yay! Fill out the parts we're interested in
        auto subscription = Subscription();
        subscription.tier = jsonResponse["tier"].get<std::string>();
        subscription.status = jsonResponse["status"].get<std::string>();
        subscription.character_count = jsonResponse["character_count"].get<uint32_t>();
        subscription.character_limit = jsonResponse["character_limit"].get<uint32_t>();
        debug("Subscription status: {}, characters left: {}", subscription.status, (subscription.character_limit - subscription.character_count));

        return subscription;
    }

}
