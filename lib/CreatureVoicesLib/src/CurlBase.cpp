
#include <algorithm>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>


#include "model/HttpMethod.h"
#include "VoiceResult.h"
#include "CurlBase.h"


namespace creatures::voice {


    size_t CurlBase::WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }


    CurlHandle CurlBase::createCurlHandle(const std::string& url) {
        debug("Creating a curl handle for URL: {}", url);
        return CurlHandle(url);
    }

    VoiceResult<std::string> CurlBase::performRequest(CurlHandle& curlHandle,
                                                            HttpMethod method,
                                                            const std::string& data) {

        debug("performing a request! method: {}, data: {}", httpMethodToString(method), data);

        std::string response;
        std::string errorMessage;

        // Make sure CURL is initialized
        if(!curlHandle.get()) {
            errorMessage = "Unable to perform request because CURL handle is not initialized";
            error(errorMessage);
            return VoiceResult<std::string>{VoiceError(VoiceError::InternalError, errorMessage)};
        }

        curl_easy_setopt(curlHandle.get(), CURLOPT_WRITEDATA, &response);
        trace("CURL handle set up for writing");

        switch (method) {
            case HttpMethod::GET:
                // GET is the default method, no need to set anything special
                break;
            case HttpMethod::POST:
                curl_easy_setopt(curlHandle.get(), CURLOPT_POST, 1L);
                curl_easy_setopt(curlHandle.get(), CURLOPT_POSTFIELDS, data.c_str());
                break;
            case HttpMethod::PUT:
                curl_easy_setopt(curlHandle.get(), CURLOPT_CUSTOMREQUEST, httpMethodToString(method).c_str());
                curl_easy_setopt(curlHandle.get(), CURLOPT_POSTFIELDS, data.c_str());
                break;
            case HttpMethod::DELETE:
                curl_easy_setopt(curlHandle.get(), CURLOPT_CUSTOMREQUEST, httpMethodToString(method).c_str());
                break;
            default:
                errorMessage = fmt::format("Unknown HTTP method: {}", httpMethodToString(method));
                error(errorMessage);
                curl_easy_cleanup(curlHandle.get());
                return VoiceResult<std::string>{VoiceError(VoiceError::InternalError, errorMessage)};
        }


        CURLcode res = curl_easy_perform(curlHandle.get());
        if (res != CURLE_OK) {
            errorMessage = fmt::format("CURL request failed: {}", curl_easy_strerror(res));
            error(errorMessage);
            curl_easy_cleanup(curlHandle.get());
            return VoiceResult<std::string>{VoiceError(VoiceError::InternalError, errorMessage)};
        }

        long http_code = 0;
        curl_easy_getinfo(curlHandle.get(), CURLINFO_RESPONSE_CODE, &http_code);

        // Let's look at the response code
        switch(http_code) {
            case 200:
            case 201:
            case 204:
                // These are all good responses
                break;
            case 301:
            case 302:
                errorMessage = fmt::format("HTTP error: {} - redirect", http_code);
                warn(errorMessage);
                curl_easy_cleanup(curlHandle.get());
                return VoiceResult<std::string>{VoiceError(VoiceError::InvalidData, errorMessage)};
            case 400:
                errorMessage = fmt::format("HTTP error: {} - bad request", http_code);
                warn(errorMessage);
                curl_easy_cleanup(curlHandle.get());
                return VoiceResult<std::string>{VoiceError(VoiceError::InvalidData, errorMessage)};
            case 401:
            case 403:
                errorMessage = fmt::format("HTTP error: {} - unauthorized", http_code);
                warn(errorMessage);
                curl_easy_cleanup(curlHandle.get());
                return VoiceResult<std::string>{VoiceError(VoiceError::InvalidApiKey, errorMessage)};
            case 404:
                errorMessage = fmt::format("HTTP error: {} - not found", http_code);
                warn(errorMessage);
                curl_easy_cleanup(curlHandle.get());
                return VoiceResult<std::string>{VoiceError(VoiceError::NotFound, errorMessage)};

                // Map everything else to an error
            default:
                errorMessage = fmt::format("HTTP error: {}", http_code);
                error(errorMessage);
                curl_easy_cleanup(curlHandle.get());
                return VoiceResult<std::string>{VoiceError(VoiceError::InternalError, errorMessage)};
        }


        // Looks good! We have good data
        curl_easy_cleanup(curlHandle.get());

        debug("request successful!");
        return VoiceResult<std::string>{response};
    }
}
