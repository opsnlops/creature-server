
#pragma once


#include <curl/curl.h>

#include <string>
#include <vector>

#include "CurlBase.h"
#include "VoiceResult.h"
#include "model/HttpMethod.h"
#include "model/Voice.h"


namespace creatures :: voice {

    class CreatureVoices : public CurlBase {
    public:
        CreatureVoices(std::string apiKey);
        VoiceResult<std::vector<Voice>> listAllAvailableVoices();

    private:
        std::string apiKey;

    };

}