
#pragma once

#include <curl/curl.h>
#include <string>
#include <spdlog/spdlog.h>

namespace creatures :: voice {

    /**
     * This is a wrapper around the curl handle to make sure we're not leaking handles over time. Since
     * we call curl_easy_init() and curl_easy_cleanup() in the constructor and destructor, respectively,
     * we can be sure that the handle is always cleaned up properly.
     */
    class CurlHandle {
    public:
        CurlHandle(const std::string& url);
        ~CurlHandle();
        //CurlHandle(CurlHandle&& other) noexcept;

        // Delete copy constructor and copy assignment to avoid double cleanup
        CurlHandle(const CurlHandle&) = delete;
        CurlHandle& operator=(const CurlHandle&) = delete;

        CURL* get() const;

    private:
        CURL* curl = nullptr;
        static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data);

    };


} // namespace creatures :: voice