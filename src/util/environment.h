
#pragma once

#include <string>

namespace creatures {

// Environment helps
int environmentToInt(const char *variable, int defaultValue);
int environmentToInt(const char *variable, const char *defaultValue);

std::string environmentToString(const char *variable,
                                const std::string &defaultValue);

} // namespace creatures