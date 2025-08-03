#pragma once

#include <iostream>
#include <optional>
#include <string>
#include <variant>

namespace creatures ::voice {

// Define the ServerError struct
class VoiceError {
  public:
    enum Code { NotFound, Forbidden, InternalError, InvalidData, InvalidApiKey };

    VoiceError(Code errorCode, const std::string &errorMessage);
    Code getCode() const;
    std::string getMessage() const;

  private:
    Code code;
    std::string message;
};

// Function to convert ServerError code to HTTP status code
int serverErrorToStatusCode(VoiceError::Code code);

// Define a generic Result type
template <typename T> class VoiceResult {
  public:
    // Constructors for success and error
    VoiceResult(const T &value);
    VoiceResult(const VoiceError &error);

    // Check if the result is a success
    [[nodiscard]] bool isSuccess() const;

    // Get the value (if success)
    std::optional<T> getValue() const;

    // Get the error (if failure)
    [[nodiscard]] std::optional<VoiceError> getError() const;

  private:
    std::variant<T, VoiceError> m_result;
};

// Implement ServerError methods
inline VoiceError::VoiceError(Code errorCode, const std::string &errorMessage)
    : code(errorCode), message(errorMessage) {}

inline VoiceError::Code VoiceError::getCode() const { return code; }

inline std::string VoiceError::getMessage() const { return message; }

// Function to convert ServerError code to HTTP status code
inline int serverErrorToStatusCode(VoiceError::Code code) {
    switch (code) {
    case VoiceError::NotFound:
        return 404;
    case VoiceError::Forbidden:
        return 403;
    case VoiceError::InternalError:
        return 500;
    case VoiceError::InvalidData:
        return 400;
    case VoiceError::InvalidApiKey:
        return 401;
    default:
        return 500;
    }
}

// Implement Result methods
template <typename T> VoiceResult<T>::VoiceResult(const T &value) : m_result(value) {}

template <typename T> VoiceResult<T>::VoiceResult(const VoiceError &error) : m_result(error) {}

template <typename T> bool VoiceResult<T>::isSuccess() const { return std::holds_alternative<T>(m_result); }

template <typename T> std::optional<T> VoiceResult<T>::getValue() const {
    if (isSuccess()) {
        return std::get<T>(m_result);
    }
    return std::nullopt;
}

template <typename T> std::optional<VoiceError> VoiceResult<T>::getError() const {
    if (!isSuccess()) {
        return std::get<VoiceError>(m_result);
    }
    return std::nullopt;
}

} // namespace creatures::voice
