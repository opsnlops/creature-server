
#pragma once


#include <nlohmann/json.hpp>

#include <string>
#include <sstream>


namespace creatures ::ws {

    enum class HttpStatus {
        OK = 200,
        Created = 201,
        Accepted = 202,
        NoContent = 204,
        BadRequest = 400,
        Unauthorized = 401,
        Forbidden = 403,
        NotFound = 404,
        MethodNotAllowed = 405,
        InternalServerError = 500,
        NotImplemented = 501,
        BadGateway = 502,
        ServiceUnavailable = 503
    };

    struct HttpResponse {
        HttpStatus status;   // HTTP status code
        std::string body;    // Serialized JSON string for the body

        // Constructor for initializing with any serializable object
        template<typename Serializable>
        HttpResponse(HttpStatus status, const Serializable &serializable)
                : status(status) {
            nlohmann::json j = serializable;
            body = j.dump(); // Serialize the object to a JSON string
        }

        // Accessors
        HttpStatus getStatus() const {
            return status;
        }

        const std::string &getBody() const {
            return body;
        }
    };

    /**
     * Convert a HttpStatus to a pair of int and string
     *
     * @param status
     * @return
     */
    inline std::pair<int, std::string> getHttpStatusMessage(HttpStatus status) {
        switch (status) {
            case HttpStatus::OK:
                return {200, "OK"};
            case HttpStatus::Created:
                return {201, "Created"};
            case HttpStatus::Accepted:
                return {202, "Accepted"};
            case HttpStatus::NoContent:
                return {204, "No Content"};
            case HttpStatus::BadRequest:
                return {400, "Bad Request"};
            case HttpStatus::Unauthorized:
                return {401, "Unauthorized"};
            case HttpStatus::Forbidden:
                return {403, "Forbidden"};
            case HttpStatus::NotFound:
                return {404, "Not Found"};
            case HttpStatus::MethodNotAllowed:
                return {405, "Method Not Allowed"};
            case HttpStatus::InternalServerError:
                return {500, "Internal Server Error"};
            case HttpStatus::NotImplemented:
                return {501, "Not Implemented"};
            case HttpStatus::BadGateway:
                return {502, "Bad Gateway"};
            case HttpStatus::ServiceUnavailable:
                return {503, "Service Unavailable"};
            default:
                return {500, "Internal Server Error"}; // Default catch-all
        }
    }

}