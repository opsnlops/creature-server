
#pragma once

#include <string>

namespace creatures::util {

/**
 * Generate a new UUID (Universally Unique Identifier)
 *
 * Uses the standard libuuid library to generate a random UUID v4.
 *
 * @return A UUID string in the format "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 */
std::string generateUUID();

} // namespace creatures::util
