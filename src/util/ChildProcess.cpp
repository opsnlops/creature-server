#include "util/ChildProcess.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <fmt/format.h>

#include "server/namespace-stuffs.h"

extern char **environ;

namespace creatures::util {

namespace {

// RAII wrapper for posix_spawn_file_actions_t
struct FileActions {
    posix_spawn_file_actions_t actions{};
    bool initialized = false;

    FileActions() {
        if (posix_spawn_file_actions_init(&actions) == 0) {
            initialized = true;
        }
    }

    ~FileActions() {
        if (initialized) {
            posix_spawn_file_actions_destroy(&actions);
        }
    }

    FileActions(const FileActions &) = delete;
    FileActions &operator=(const FileActions &) = delete;
};

// Build the argv array for posix_spawn. argv[0] is the program name;
// posix_spawn expects a NULL-terminated array of char *. We hold the
// underlying C strings in a vector<string> so they live long enough.
struct ArgvBuffer {
    std::vector<std::string> storage;
    std::vector<char *> pointers;

    ArgvBuffer(const std::string &binary, const std::vector<std::string> &args) {
        storage.reserve(args.size() + 1);
        storage.push_back(binary);
        for (const auto &a : args) {
            storage.push_back(a);
        }
        pointers.reserve(storage.size() + 1);
        for (auto &s : storage) {
            pointers.push_back(s.data());
        }
        pointers.push_back(nullptr);
    }
};

} // namespace

Result<ChildProcessResult> runChildProcess(const std::string &binary, const std::vector<std::string> &args,
                                           bool mergeStderrToStdout, ChildProcessLineCallback onLine) {
    // pipe for stdout (and optionally stderr).
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        return Result<ChildProcessResult>{
            ServerError(ServerError::InternalError, fmt::format("pipe() failed: {}", strerror(errno)))};
    }

    FileActions fa;
    if (!fa.initialized) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return Result<ChildProcessResult>{
            ServerError(ServerError::InternalError, "posix_spawn_file_actions_init failed")};
    }

    // Child: close read end, dup write end onto stdout (and stderr if requested), then close the original write end.
    if (posix_spawn_file_actions_addclose(&fa.actions, pipeFds[0]) != 0 ||
        posix_spawn_file_actions_adddup2(&fa.actions, pipeFds[1], STDOUT_FILENO) != 0 ||
        (mergeStderrToStdout && posix_spawn_file_actions_adddup2(&fa.actions, pipeFds[1], STDERR_FILENO) != 0) ||
        posix_spawn_file_actions_addclose(&fa.actions, pipeFds[1]) != 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return Result<ChildProcessResult>{
            ServerError(ServerError::InternalError, "posix_spawn_file_actions setup failed")};
    }

    ArgvBuffer argv(binary, args);

    pid_t childPid = 0;
    int spawnRc = posix_spawnp(&childPid, binary.c_str(), &fa.actions, nullptr, argv.pointers.data(), environ);
    if (spawnRc != 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return Result<ChildProcessResult>{ServerError(
            ServerError::InternalError, fmt::format("posix_spawnp('{}') failed: {}", binary, strerror(spawnRc)))};
    }

    // Parent: close the write end so the read side gets EOF when the child exits.
    ::close(pipeFds[1]);

    ChildProcessResult result;
    std::string lineAccumulator;
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(pipeFds[0], buffer, sizeof(buffer))) > 0) {
        result.output.append(buffer, static_cast<size_t>(n));
        if (onLine) {
            lineAccumulator.append(buffer, static_cast<size_t>(n));
            // Emit each complete line as we get it.
            size_t pos = 0;
            while (true) {
                size_t newlinePos = lineAccumulator.find('\n', pos);
                if (newlinePos == std::string::npos) {
                    break;
                }
                onLine(lineAccumulator.substr(pos, newlinePos - pos + 1));
                pos = newlinePos + 1;
            }
            if (pos > 0) {
                lineAccumulator.erase(0, pos);
            }
        }
    }
    ::close(pipeFds[0]);

    // Flush any unterminated trailing chunk to the line callback.
    if (onLine && !lineAccumulator.empty()) {
        onLine(lineAccumulator);
    }

    int status = 0;
    if (::waitpid(childPid, &status, 0) < 0) {
        return Result<ChildProcessResult>{
            ServerError(ServerError::InternalError, fmt::format("waitpid({}) failed: {}", childPid, strerror(errno)))};
    }

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        // Map signal-terminated child to a non-zero exit code in the conventional shell style.
        result.exitCode = 128 + WTERMSIG(status);
    } else {
        result.exitCode = -1;
    }

    return Result<ChildProcessResult>{result};
}

} // namespace creatures::util
