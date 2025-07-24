
#include "environment.h"

#include "server/namespace-stuffs.h"

namespace creatures {

int environmentToInt(const char *variable, int defaultValue) {
    trace("converting {} to an int from the environment (default is {})",
          variable, defaultValue);

    int value;
    const char *valueString = std::getenv(variable);
    if (valueString != nullptr) {

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

int environmentToInt(const char *variable, const char *defaultValue) {
    return environmentToInt(variable, std::stoi(std::string(defaultValue)));
}

std::string environmentToString(const char *variable,
                                const std::string &defaultValue) {
    trace("getting {} from the environment (default is {})", variable,
          defaultValue);

    const char *valueString = std::getenv(variable);
    if (valueString != nullptr && valueString[0] != '\0') {
        trace("environment var {} is {}", variable, valueString);
        return {valueString};
    } else {
        trace("using the default of {}", defaultValue);
        return defaultValue;
    }
}

} // namespace creatures