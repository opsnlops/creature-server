
#pragma once

#include <string>

namespace creatures {

// Environment helps
int environmentToInt(const char *variable, int defaultValue);
int environmentToInt(const char *variable, const char *defaultValue);

std::string environmentToString(const char *variable, const std::string &defaultValue);

double environmentToDouble(const char *variable, double defaultValue);

/// Unsigned 64-bit variant, for byte-sized knobs. Same parse-or-default-and-log
/// contract as the rest of this family (issue #93 review).
unsigned long long environmentToUnsignedLongLong(const char *variable, unsigned long long defaultValue);

} // namespace creatures