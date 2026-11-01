
#pragma once

#include <string>

namespace creatures {

/**
 * Is this variable set to an actual value?
 *
 * Empty counts as UNSET, which is what the rest of this family treats it as,
 * and what a systemd EnvironmentFile line of `FOO=` means in practice. Exists
 * because callers used to ask `getenv(...) != nullptr` and disagree with the
 * parsers about it — an empty RTP_AUDIO_LOAD_WORKERS= looked like an operator
 * override to the caller while the parser rejected the value (issue #242).
 */
bool environmentSet(const char *variable);

// Environment helps
int environmentToInt(const char *variable, int defaultValue);
int environmentToInt(const char *variable, const char *defaultValue);

std::string environmentToString(const char *variable, const std::string &defaultValue);

double environmentToDouble(const char *variable, double defaultValue);

/// Unsigned 64-bit variant, for byte-sized knobs. Same parse-or-default-and-log
/// contract as the rest of this family (issue #93 review).
unsigned long long environmentToUnsignedLongLong(const char *variable, unsigned long long defaultValue);

} // namespace creatures