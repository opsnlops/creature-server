#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/Result.h"

namespace creatures {

/**
 * A JSON configuration file, read before the command line is parsed.
 *
 * Exists so the production settings can live in /etc/creature/server.json,
 * version-controlled and shipped by the package, instead of only inside a
 * systemd unit — which means changing them does not mean editing a unit file
 * and running daemon-reload. Matches how creature-world is configured
 * (issue #237).
 *
 * Precedence is **config file < environment < command line**. This class is
 * only the bottom layer: its values become the defaults argparse falls back on,
 * so an environment variable still overrides the file and an explicit flag
 * still overrides both. That ordering matters — the flag has to win for one-off
 * runs, and the environment has to keep working for anything already relying
 * on it.
 *
 * Unknown keys are an ERROR rather than ignored. A typo'd setting that silently
 * does nothing is worse than a server that refuses to start and says why; the
 * JSON codecs elsewhere in the project take the same line. Keys beginning with
 * an underscore are ignored, which is how a file humans edit gets comments in a
 * format that has none.
 */
class ConfigFile {
  public:
    /// Every key this server understands. A key not listed here is rejected at
    /// load time; adding a setting means adding it here and reading it at its
    /// one call site in CommandLine.
    static const std::vector<std::string> &knownKeys();

    /// An empty config — every lookup falls through to its fallback. Used when
    /// no file is present, which is the normal case for a dev build.
    static ConfigFile empty() { return ConfigFile{}; }

    /**
     * Read and validate a config file.
     *
     * @param path file to read.
     * @param required when false, a missing file yields an empty config rather
     *        than an error. The default path is read opportunistically, but a
     *        path given explicitly with --config must exist — asking for a file
     *        that isn't there is a mistake worth reporting.
     */
    static Result<ConfigFile> load(const std::string &path, bool required);

    bool has(std::string_view key) const;

    /// Typed lookups. A key present with the wrong JSON type is reported at
    /// load time, so these cannot fail here.
    std::string stringOr(std::string_view key, const std::string &fallback) const;
    int intOr(std::string_view key, int fallback) const;
    bool boolOr(std::string_view key, bool fallback) const;

    /// Where this came from, for the startup log. Empty when nothing was read.
    const std::string &sourcePath() const { return sourcePath_; }

  private:
    ConfigFile() = default;

    nlohmann::json data_ = nlohmann::json::object();
    std::string sourcePath_;
};

} // namespace creatures
