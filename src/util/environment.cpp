
#include "environment.h"

#include "server/namespace-stuffs.h"

namespace creatures {

bool environmentSet(const char *variable) {
    const char *valueString = std::getenv(variable);
    return valueString != nullptr && valueString[0] != '\0';
}

int environmentToInt(const char *variable, int defaultValue) {
    trace("converting {} to an int from the environment (default is {})", variable, defaultValue);

    int value;
    const char *valueString = std::getenv(variable);
    // Empty is UNSET, not malformed. std::stoi("") throws, so this used to log
    // "<var> is not an int?" for a bare `FOO=` line — which the packaged
    // /etc/default/creature-server makes easy to leave behind when uncommenting
    // a setting. environmentToString and environmentToUnsignedLongLong already
    // treated empty this way (issue #242).
    if (valueString != nullptr && valueString[0] != '\0') {

        try {

            value = std::stoi(std::string(valueString));
            trace("environment var {} is {}", variable, value);
            return value;

        } catch (std::invalid_argument &e) {
            error("{} is not an int?", variable);
            return defaultValue;
        } catch (std::out_of_range &e) {
            error("{} is out of range", variable);
            return defaultValue;
        }
    } else {
        trace("using the default of {}", defaultValue);
        return defaultValue;
    }
}

unsigned long long environmentToUnsignedLongLong(const char *variable, unsigned long long defaultValue) {
    trace("converting {} to an unsigned long long from the environment (default is {})", variable, defaultValue);

    const char *valueString = std::getenv(variable);
    if (valueString == nullptr || valueString[0] == '\0') {
        trace("using the default of {}", defaultValue);
        return defaultValue;
    }

    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(std::string(valueString), &consumed);
        if (consumed != std::string(valueString).size()) {
            error("{} is not a plain unsigned number", variable);
            return defaultValue;
        }
        trace("environment var {} is {}", variable, value);
        return value;
    } catch (std::invalid_argument &e) {
        error("{} is not an unsigned long long?", variable);
        return defaultValue;
    } catch (std::out_of_range &e) {
        error("{} is out of range", variable);
        return defaultValue;
    }
}

int environmentToInt(const char *variable, const char *defaultValue) {
    return environmentToInt(variable, std::stoi(std::string(defaultValue)));
}

std::string environmentToString(const char *variable, const std::string &defaultValue) {
    trace("getting {} from the environment (default is {})", variable, defaultValue);

    const char *valueString = std::getenv(variable);
    if (valueString != nullptr && valueString[0] != '\0') {
        trace("environment var {} is {}", variable, valueString);
        return {valueString};
    } else {
        trace("using the default of {}", defaultValue);
        return defaultValue;
    }
}

double environmentToDouble(const char *variable, double defaultValue) {
    trace("converting {} to a double from the environment (default is {})", variable, defaultValue);

    double value;
    const char *valueString = std::getenv(variable);
    // Empty is UNSET, same as the rest of this family (issue #242).
    if (valueString != nullptr && valueString[0] != '\0') {

        try {

            value = std::stod(std::string(valueString));
            trace("environment var {} is {}", variable, value);
            return value;

        } catch (std::invalid_argument &e) {
            error("{} is not a double?", variable);
            return defaultValue;
        } catch (std::out_of_range &e) {
            error("{} is out of range", variable);
            return defaultValue;
        }
    } else {
        trace("using the default of {}", defaultValue);
        return defaultValue;
    }
}

} // namespace creatures