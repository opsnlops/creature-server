#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include "server/config/ConfigFile.h"
#include "util/helpers.h"

namespace creatures {

const std::vector<std::string> &ConfigFile::knownKeys() {
    // Deliberately a short list rather than all 34 command-line settings: these
    // are the ones the production unit actually sets, so they are the ones
    // worth moving out of it. Adding another is a line here plus one call site.
    static const std::vector<std::string> keys{
        "mongodb_uri", "sounds_location", "audio_cache_dir", "network_device", "audio_mode", "sound_channels",
    };
    return keys;
}

namespace {

/// Underscore-prefixed keys are comments. JSON has none, and a file people edit
/// by hand needs somewhere to explain itself.
bool isCommentKey(const std::string &key) { return !key.empty() && key.front() == '_'; }

} // namespace

Result<ConfigFile> ConfigFile::load(const std::string &path, bool required) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        if (required) {
            return Result<ConfigFile>{ServerError(
                ServerError::NotFound, fmt::format("Config file {} does not exist (asked for with --config)", path))};
        }
        return Result<ConfigFile>{ConfigFile{}};
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<ConfigFile>{
            ServerError(ServerError::NotFound, fmt::format("Config file {} could not be opened", path))};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(buffer.str());
    } catch (const std::exception &e) {
        return Result<ConfigFile>{
            ServerError(ServerError::InvalidData, fmt::format("Config file {} is not valid JSON: {}", path, e.what()))};
    }

    if (!parsed.is_object()) {
        return Result<ConfigFile>{
            ServerError(ServerError::InvalidData, fmt::format("Config file {} must contain a JSON object", path))};
    }

    // Reject anything we don't understand, naming the key AND what we do
    // accept — a startup failure is only useful if it says how to fix itself.
    std::vector<std::string> unknown;
    for (const auto &[key, value] : parsed.items()) {
        if (isCommentKey(key)) {
            continue;
        }
        const auto &keys = knownKeys();
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            unknown.push_back(key);
        }
    }
    if (!unknown.empty()) {
        return Result<ConfigFile>{ServerError(
            ServerError::InvalidData, fmt::format("Config file {} has unknown setting(s): {}. Known settings are: {}",
                                                  path, joinStrings(unknown, ", "), joinStrings(knownKeys(), ", ")))};
    }

    // Type-check now rather than at first use, so a bad value is reported at
    // startup instead of whenever that setting happens to be read.
    for (const auto &[key, value] : parsed.items()) {
        if (isCommentKey(key)) {
            continue;
        }
        const bool typeOk = (key == "sound_channels") ? value.is_number_integer() : value.is_string();
        if (!typeOk) {
            return Result<ConfigFile>{ServerError(
                ServerError::InvalidData, fmt::format("Config file {}: setting '{}' should be {}", path, key,
                                                      key == "sound_channels" ? "a whole number" : "a string"))};
        }
    }

    ConfigFile config;
    config.data_ = std::move(parsed);
    config.sourcePath_ = path;
    return Result<ConfigFile>{std::move(config)};
}

bool ConfigFile::has(std::string_view key) const { return data_.contains(std::string(key)); }

std::string ConfigFile::stringOr(std::string_view key, const std::string &fallback) const {
    const auto it = data_.find(std::string(key));
    if (it == data_.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

int ConfigFile::intOr(std::string_view key, int fallback) const {
    const auto it = data_.find(std::string(key));
    if (it == data_.end() || !it->is_number_integer()) {
        return fallback;
    }
    return it->get<int>();
}

bool ConfigFile::boolOr(std::string_view key, bool fallback) const {
    const auto it = data_.find(std::string(key));
    if (it == data_.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

} // namespace creatures
