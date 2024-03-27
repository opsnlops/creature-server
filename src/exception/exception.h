
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

    class NotFoundException : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit NotFoundException(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit NotFoundException(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~NotFoundException() noexcept {}

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

    class InvalidArgumentException : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit InvalidArgumentException(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit InvalidArgumentException(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~InvalidArgumentException() noexcept {}

        // Override the what() function to return the error message
        virtual const char* what() const noexcept {
            return message_.c_str();
        }

    private:
        std::string message_;
    };

    class DatabaseError : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit DatabaseError(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit DatabaseError(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~DatabaseError() noexcept {}

        // Override the what() function to return the error message
        virtual const char* what() const noexcept {
            return message_.c_str();
        }

    private:
        std::string message_;
    };

    class DuplicateFoundError : public std::exception {
    public:
        // Constructor that takes an error message string
        explicit DuplicateFoundError(const std::string& message)
                : message_(message) {}

        // Constructor that takes a C-style string error message
        explicit DuplicateFoundError(const char* message)
                : message_(message) {}

        // Destructor
        virtual ~DuplicateFoundError() noexcept {}

        // Override the what() function to return the error message
        virtual const char* what() const noexcept {
            return message_.c_str();
        }

    private:
        std::string message_;
    };

}