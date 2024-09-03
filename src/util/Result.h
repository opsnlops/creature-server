#pragma once

#include <variant>
#include <optional>
#include <string>
#include <iostream>

namespace creatures {

// Define the ServerError struct
    class ServerError {
    public:
        enum Code {
            NotFound,
            Forbidden,
            InternalError,
            InvalidData,
            DatabaseError
        };

        ServerError(Code code, const std::string& message);
        Code getCode() const;
        std::string getMessage() const;

    private:
        Code code;
        std::string message;
    };

// Function to convert ServerError code to HTTP status code
    int serverErrorToStatusCode(ServerError::Code code);

// Define a generic Result type
    template <typename T>
    class Result {
    public:
        // Constructors for success and error
        Result(const T& value);
        Result(const ServerError& error);

        // Check if the result is a success
        [[nodiscard]] bool isSuccess() const;

        // Get the value (if success)
        std::optional<T> getValue() const;

        // Get the error (if failure)
        [[nodiscard]] std::optional<ServerError> getError() const;

    private:
        std::variant<T, ServerError> m_result;
    };

// Implement ServerError methods
    inline ServerError::ServerError(Code code, const std::string& message) : code(code), message(message) {}

    inline ServerError::Code ServerError::getCode() const {
        return code;
    }

    inline std::string ServerError::getMessage() const {
        return message;
    }

// Function to convert ServerError code to HTTP status code
    inline int serverErrorToStatusCode(ServerError::Code code) {
        switch (code) {
            case ServerError::NotFound: return 404;
            case ServerError::Forbidden: return 403;
            case ServerError::InvalidData: return 400;
            default: return 500;
        }
    }

// Implement Result methods
    template <typename T>
    Result<T>::Result(const T& value) : m_result(value) {}

    template <typename T>
    Result<T>::Result(const ServerError& error) : m_result(error) {}

    template <typename T>
    bool Result<T>::isSuccess() const {
        return std::holds_alternative<T>(m_result);
    }

    template <typename T>
    std::optional<T> Result<T>::getValue() const {
        if (isSuccess()) {
            return std::get<T>(m_result);
        }
        return std::nullopt;
    }

    template <typename T>
    std::optional<ServerError> Result<T>::getError() const {
        if (!isSuccess()) {
            return std::get<ServerError>(m_result);
        }
        return std::nullopt;
    }

}
