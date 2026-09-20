#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

/**
 * A file the transport streams to the client in bounded chunks. The
 * application worker only resolves, validates, and sizes the file; it never
 * reads the bytes. The loop thread never reads them either: it hands each
 * chunk read to the file executor and writes what comes back.
 */
struct FilePayload {
    std::string path;
    std::uint64_t size{0};
};

/** A response value that never owns or refers to a transport socket. */
struct PreparedResponse {
    int statusCode{200};
    std::string contentType;
    std::string body;
    std::vector<HttpHeader> headers;
    /** When set, `body` is empty and the transport streams this file instead. */
    std::optional<FilePayload> file;

    static PreparedResponse json(int statusCode_, std::string body_) {
        return {.statusCode = statusCode_,
                .contentType = "application/json; charset=utf-8",
                .body = std::move(body_),
                .headers = {},
                .file = std::nullopt};
    }

    static PreparedResponse bytes(int statusCode_, std::string contentType_, std::string body_,
                                  std::vector<HttpHeader> headers_ = {}) {
        return {.statusCode = statusCode_,
                .contentType = std::move(contentType_),
                .body = std::move(body_),
                .headers = std::move(headers_),
                .file = std::nullopt};
    }

    static PreparedResponse fileStream(std::string path_, std::uint64_t size_, std::string contentType_,
                                       std::vector<HttpHeader> headers_ = {}) {
        return {.statusCode = 200,
                .contentType = std::move(contentType_),
                .body = {},
                .headers = std::move(headers_),
                .file = FilePayload{.path = std::move(path_), .size = size_}};
    }

    /** Bytes the client will receive, whether from `body` or from the file. */
    [[nodiscard]] std::uint64_t contentLength() const { return file ? file->size : body.size(); }
};

} // namespace creatures::transport
