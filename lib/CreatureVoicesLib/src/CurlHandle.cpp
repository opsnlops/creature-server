
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
        if (headers) {
            curl_slist_free_all(headers);
        }
        spdlog::trace("CurlHandle destroyed");
    }


    CurlHandle::CurlHandle(CurlHandle&& other) noexcept : curl(other.curl), headers(other.headers) {
        other.curl = nullptr;
        other.headers = nullptr;
    }

    CurlHandle& CurlHandle::operator=(CurlHandle&& other) noexcept {
        if (this != &other) {
            if (curl) {
                curl_easy_cleanup(curl);
            }
            if (headers) {
                curl_slist_free_all(headers);
            }
            spdlog::trace("CurlHandle destroyed via move assignment");
            curl = other.curl;
            headers = other.headers;
            other.curl = nullptr;
            other.headers = nullptr;
        }
        return *this;
    }

    CURL* CurlHandle::get() const {
        return curl;
    }

    void CurlHandle::addHeader(const std::string& header) {
        headers = curl_slist_append(headers, header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    size_t CurlHandle::WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
        data->append(ptr, size * nmemb);
        return size * nmemb;
    }

}