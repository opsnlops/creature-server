
#pragma once

#include <stdexcept>
#include <string>

namespace creatures {

    class DataFormatException : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit DataFormatException(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit DataFormatException(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~DataFormatException() noexcept {}

        // Override the what() function to return the error message
        virtual const char* what() const noexcept {
            return message_.c_str();
        }

    private:
        std::string message_;
    };

    class CreatureNotFoundException : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit CreatureNotFoundException(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit CreatureNotFoundException(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~CreatureNotFoundException() noexcept {}

        // Override the what() function to return the error message
        virtual const char* what() const noexcept {
            return message_.c_str();
        }

    private:
        std::string message_;
    };

    class InternalError : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit InternalError(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit InternalError(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~InternalError() noexcept {}

        // Override the what() function to return the error message
        virtual const char* what() const noexcept {
            return message_.c_str();
        }

    private:
        std::string message_;
    };


}