
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

        // Allow move semantics
        CurlHandle(CurlHandle&& other) noexcept;

        // Delete copy constructor and copy assignment to avoid double cleanup
        CurlHandle(const CurlHandle&) = delete;
        CurlHandle& operator=(const CurlHandle&) = delete;

        CurlHandle& operator=(CurlHandle&& other) noexcept;

        CURL* get() const;

        void addHeader(const std::string& header);

    private:
        CURL* curl = nullptr;
        struct curl_slist* headers = nullptr;
        static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data);

    };


} // namespace creatures :: voice