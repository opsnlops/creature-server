#pragma once

#include <functional>
#include <string>
#include <vector>

#include "util/Result.h"

namespace creatures::util {

struct ChildProcessResult {
    int exitCode = 0;
    std::string output;
};

// Called once per newline-terminated chunk of stdout. The line argument
// includes the trailing newline if one was present. Useful for streaming
// progress parsers (e.g. Rhubarb's --machineReadable JSON-per-line).
using ChildProcessLineCallback = std::function<void(const std::string &)>;

// Spawn `binary` with the given argv, capture stdout (and stderr if
// mergeStderrToStdout is true), wait for it to exit, and return the
// exit code + accumulated output. Uses posix_spawn + an argv array
// rather than /bin/sh, so filenames in `args` are never interpreted
// as shell metacharacters — this is the whole point of the helper.
//
// If onLine is provided, it's invoked for each newline-terminated
// chunk read from the child's stdout, in addition to being
// accumulated into the returned `output`.
//
// Returns an InternalError result if the spawn itself fails. The
// child's non-zero exit code is reported in the result, not as an
// error — callers decide how to interpret it.
Result<ChildProcessResult> runChildProcess(const std::string &binary, const std::vector<std::string> &args,
                                           bool mergeStderrToStdout = true, ChildProcessLineCallback onLine = nullptr);

} // namespace creatures::util
