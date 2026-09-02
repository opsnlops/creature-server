#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace creatures::transport {

enum class BodyPolicyKind {
    None,
    Json,
    Upload,
};

struct BodyPolicy {
    BodyPolicyKind kind{BodyPolicyKind::None};
    std::size_t maximumBytes{0};
};

struct HttpHeader {
    std::string name;
    std::string value;
};

/** A response value that never owns or refers to a transport socket. */
struct PreparedResponse {
    int statusCode{200};
    std::string contentType;
    std::string body;
    std::vector<HttpHeader> headers;

    static PreparedResponse json(int statusCode_, std::string body_) {
        return {.statusCode = statusCode_,
                .contentType = "application/json; charset=utf-8",
                .body = std::move(body_),
                .headers = {}};
    }
};

} // namespace creatures::transport
