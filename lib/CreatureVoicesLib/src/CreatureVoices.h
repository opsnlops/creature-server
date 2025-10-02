
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "CurlBase.h"
#include "VoiceResult.h"
#include "model/CreatureSpeechRequest.h"
#include "model/CreatureSpeechResponse.h"
#include "model/HttpMethod.h"
#include "model/Subscription.h"
#include "model/Voice.h"

#define VOICES_API_BASE_URL "https://api.elevenlabs.io"

namespace creatures ::voice {

class CreatureVoices : public CurlBase {
  public:
    CreatureVoices(std::string apiKey_);
    VoiceResult<std::vector<Voice>> listAllAvailableVoices();

    VoiceResult<Subscription> getSubscriptionStatus();

    /**
     * Create a new sound file for a creature based on the text given
     *
     * @param fileSavePath the location to save the file
     * @param speechRequest the request to generate the speech
     * @return A VoiceResult with information about what happened
     */
    VoiceResult<CreatureSpeechResponse> generateCreatureSpeech(const std::filesystem::path &fileSavePath,
                                                               const CreatureSpeechRequest &speechRequest);

  private:
    std::string apiKey;

    std::string makeFileName(const CreatureSpeechRequest &speechRequest);
    std::string toLowerAndReplaceSpaces(std::string str);
};

} // namespace creatures::voice