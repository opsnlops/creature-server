
#pragma once

/**
 * Simple enum to make it easier to work with HTTP methods
 */
namespace creatures :: voice {
    enum class HttpMethod {
        GET,
        POST,
        PUT,
        DELETE
    };

    std::string httpMethodToString(HttpMethod method);
}