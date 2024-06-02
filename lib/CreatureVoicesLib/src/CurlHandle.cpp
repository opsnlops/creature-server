
#include <curl/curl.h>
#include <string>
#include <spdlog/spdlog.h>

#include "CurlHandle.h"


namespace creatures :: voice {

    CurlHandle::CurlHandle(const std::string& url) {
        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        } else {
            spdlog::error("Failed to initialize curl handle");
        }
    }

    /**
     * Make sure to clean up the curl handle when the object is destroyed.
     */
    CurlHandle::~CurlHandle() {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }


    CURL* CurlHandle::get() const {
        return curl;
    }

    size_t CurlHandle::WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }

}