#pragma once

#include <string>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "creature/logging/Logger.h"

namespace creatures {

/**
 * Adapts creature-common's injected Logger onto the server's spdlog
 *
 * The shared libraries log through an interface so they don't have to know who
 * is listening (issue #255). The server logs through spdlog's free functions,
 * so this forwards one to the other.
 *
 * `init()` and `setLevel()` are deliberately no-ops: the server configures
 * spdlog once in main(), and a library is not entitled to change the whole
 * process's log level out from under it.
 */
class SharedLibraryLogger final : public Logger {
  public:
    explicit SharedLibraryLogger(std::string prefix) : prefix_(std::move(prefix)) {}

    void init(std::string) override {}
    void setLevel(const std::string &) override {}

  protected:
    void logTrace(const std::string &format, fmt::format_args args) override {
        spdlog::trace("{}{}", prefix_, fmt::vformat(format, args));
    }
    void logDebug(const std::string &format, fmt::format_args args) override {
        spdlog::debug("{}{}", prefix_, fmt::vformat(format, args));
    }
    void logInfo(const std::string &format, fmt::format_args args) override {
        spdlog::info("{}{}", prefix_, fmt::vformat(format, args));
    }
    void logWarning(const std::string &format, fmt::format_args args) override {
        spdlog::warn("{}{}", prefix_, fmt::vformat(format, args));
    }
    void logError(const std::string &format, fmt::format_args args) override {
        spdlog::error("{}{}", prefix_, fmt::vformat(format, args));
    }
    void logCritical(const std::string &format, fmt::format_args args) override {
        spdlog::critical("{}{}", prefix_, fmt::vformat(format, args));
    }

  private:
    std::string prefix_;
};

} // namespace creatures
