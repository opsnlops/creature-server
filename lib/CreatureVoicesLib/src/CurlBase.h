
#pragma once

#include <string>
#include <vector>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "CurlHandle.h"
#include "VoiceResult.h"
#include "model/HttpMethod.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

namespace creatures :: voice {

    class CurlBase {
    public:
        CurlBase() = default;
        ~CurlBase() = default;

    protected:
        CurlHandle createCurlHandle(const std::string& url);
        VoiceResult<std::string> performRequest(CurlHandle& curlHandle, HttpMethod method, const std::string& data);
        static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data);
    };

}