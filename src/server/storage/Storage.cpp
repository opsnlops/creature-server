#include "Storage.h"

#include <fstream>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "model/CacheInvalidation.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
} // namespace creatures

namespace creatures::storage {

namespace {

// Subdir names under the system temp root. Mirror what JobWorker.cpp +
// DialogCache.cpp had inline before this facade existed — keeping the same
// directory names means existing TTL crons + in-flight files keep working
// across the cutover.
constexpr const char *kAdHocSubdir = "creature-adhoc";
constexpr const char *kJobScratchSubdir = "creature-lipsync";
constexpr const char *kGenerationCacheSubdir = "creature-adhoc/dialog-cache";

// Compute the root path for a persistence bucket WITHOUT creating it.
std::filesystem::path bareRoot(Persistence persistence) {
    switch (persistence) {
    case Persistence::Permanent:
        return std::filesystem::path(creatures::config ? creatures::config->getSoundFileLocation() : std::string{});
    case Persistence::AdHoc:
        return std::filesystem::temp_directory_path() / kAdHocSubdir;
    case Persistence::JobScratch:
        return std::filesystem::temp_directory_path() / kJobScratchSubdir;
    case Persistence::GenerationCache:
        return std::filesystem::temp_directory_path() / kGenerationCacheSubdir;
    }
    return {};
}

// For the StoragePath.forMetadata field: Permanent is stored as a relative
// path (so the deployment can move the sound root without rewriting the DB);
// everything else is absolute (no resolver could find them otherwise).
std::string metadataPathFor(Persistence persistence, const std::filesystem::path &absolute,
                            const std::filesystem::path &root) {
    if (persistence != Persistence::Permanent) {
        return absolute.string();
    }
    std::error_code ec;
    const auto relative = std::filesystem::relative(absolute, root, ec);
    if (ec || relative.empty()) {
        warn("storage::metadataPathFor: relative({}, {}) failed: {}; falling back to absolute", absolute.string(),
             root.string(), ec.message());
        return absolute.string();
    }
    return relative.string();
}

// What CacheType (if any) should fire after a successful write to this bucket.
std::optional<CacheType> soundInvalidationFor(Persistence persistence) {
    switch (persistence) {
    case Persistence::Permanent:
        return CacheType::SoundList;
    case Persistence::AdHoc:
        return CacheType::AdHocSoundList;
    case Persistence::JobScratch:
    case Persistence::GenerationCache:
        return std::nullopt;
    }
    return std::nullopt;
}

// Single .tmp + rename writer. Mirrors DialogCache::saveGeneration's pattern:
// open .tmp, write, flush, check, rename. On any failure clean up the .tmp.
Result<void> atomicWrite(const std::filesystem::path &target, std::span<const std::uint8_t> bytes) {
    const auto tmp = target.string() + ".tmp";
    std::error_code ec;

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Result<void>{ServerError(ServerError::InternalError,
                                            fmt::format("storage::atomicWrite: open {} for write failed", tmp))};
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return Result<void>{
                ServerError(ServerError::InternalError, fmt::format("storage::atomicWrite: write {} failed", tmp))};
        }
    }

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Result<void>{
            ServerError(ServerError::InternalError, fmt::format("storage::atomicWrite: rename {} → {} failed: {}", tmp,
                                                                target.string(), ec.message()))};
    }
    return Result<void>{};
}

// Shared template for the publisher pattern: call a db->* method, fire the
// invalidation(s) only on success, return the underlying Result. Used by
// every publishX / deleteX / republishAnimation function below.
//
// The invalidation MUST NOT fire on failure — the contract is "this is a
// successful publish/delete event," and clients refreshing on a failed call
// would just re-fetch the unchanged data, which is wasteful but worse: it
// could mask transient state.
template <typename DbCall, typename... Caches> auto runPublisher(const char *opName, DbCall &&call, Caches... caches) {
    using ResultType = std::invoke_result_t<DbCall>;
    if (!creatures::db) {
        return ResultType{ServerError(ServerError::InternalError, fmt::format("storage::{}: db unavailable", opName))};
    }
    auto result = call();
    if (result.isSuccess()) {
        (scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, caches), ...);
    }
    return result;
}

} // namespace

Result<std::filesystem::path> root(Persistence persistence) {
    auto p = bareRoot(persistence);
    if (p.empty()) {
        return Result<std::filesystem::path>{ServerError(
            ServerError::InternalError, "storage::root: empty root path (config not initialized for Permanent?)")};
    }
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    if (ec) {
        return Result<std::filesystem::path>{ServerError(
            ServerError::InternalError, fmt::format("storage::root: mkdir {} failed: {}", p.string(), ec.message()))};
    }
    return Result<std::filesystem::path>{p};
}

Result<StoragePath> allocateSoundPath(Persistence persistence, std::string filename,
                                      std::optional<std::string> subdir) {
    if (filename.empty()) {
        return Result<StoragePath>{ServerError(ServerError::InvalidData, "storage::allocateSoundPath: filename empty")};
    }
    auto rootResult = root(persistence);
    if (!rootResult.isSuccess()) {
        return Result<StoragePath>{rootResult.getError().value()};
    }
    auto rootPath = rootResult.getValue().value();
    auto parent = subdir ? (rootPath / *subdir) : rootPath;

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        return Result<StoragePath>{
            ServerError(ServerError::InternalError,
                        fmt::format("storage::allocateSoundPath: mkdir {} failed: {}", parent.string(), ec.message()))};
    }

    StoragePath sp;
    sp.absolute = parent / filename;
    sp.forMetadata = metadataPathFor(persistence, sp.absolute, rootPath);
    return Result<StoragePath>{sp};
}

Result<StoragePath> writeSoundFile(Persistence persistence, std::string filename, std::span<const std::uint8_t> bytes,
                                   std::optional<std::string> subdir) {
    auto pathResult = allocateSoundPath(persistence, std::move(filename), std::move(subdir));
    if (!pathResult.isSuccess()) {
        return Result<StoragePath>{pathResult.getError().value()};
    }
    auto sp = pathResult.getValue().value();

    auto writeResult = atomicWrite(sp.absolute, bytes);
    if (!writeResult.isSuccess()) {
        return Result<StoragePath>{writeResult.getError().value()};
    }

    if (auto cache = soundInvalidationFor(persistence); cache.has_value()) {
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, *cache);
    }
    return Result<StoragePath>{sp};
}

Result<creatures::Animation> publishAnimation(const std::string &animationJson,
                                              std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishAnimation", [&] { return creatures::db->upsertAnimation(animationJson, parentSpan); },
        CacheType::Animation, CacheType::SoundList);
}

Result<void> publishAdHocAnimation(const creatures::Animation &animation, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishAdHocAnimation",
        [&] { return creatures::db->insertAdHocAnimation(animation, std::chrono::system_clock::now(), parentSpan); },
        CacheType::AdHocAnimationList, CacheType::AdHocSoundList);
}

Result<creatures::Animation> republishAnimation(const std::string &animationJson,
                                                std::shared_ptr<OperationSpan> parentSpan) {
    // Animation only — no SoundList invalidation because the sound file
    // reference didn't change (the lipsync handler mutates tracks in-place
    // on the existing animation's existing sound).
    return runPublisher(
        "republishAnimation", [&] { return creatures::db->upsertAnimation(animationJson, parentSpan); },
        CacheType::Animation);
}

Result<creatures::Creature> publishCreature(const std::string &creatureJson,
                                            std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishCreature", [&] { return creatures::db->upsertCreature(creatureJson, parentSpan); },
        CacheType::Creature);
}

Result<creatures::DmxFixture> publishFixture(const std::string &fixtureJson,
                                             std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishFixture", [&] { return creatures::db->upsertFixture(fixtureJson, parentSpan); }, CacheType::Fixture);
}

Result<void> deleteFixture(const fixtureId_t &fixtureId, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "deleteFixture", [&] { return creatures::db->deleteFixture(fixtureId, parentSpan); }, CacheType::Fixture);
}

Result<void> setFixtureUniverse(const fixtureId_t &fixtureId, std::optional<universe_t> universe,
                                std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "setFixtureUniverse", [&] { return creatures::db->setFixtureUniverse(fixtureId, universe, parentSpan); },
        CacheType::Fixture);
}

Result<creatures::Playlist> publishPlaylist(const std::string &playlistJson,
                                            std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishPlaylist", [&] { return creatures::db->upsertPlaylist(playlistJson, parentSpan); },
        CacheType::Playlist);
}

Result<creatures::DialogScript> publishDialogScript(const std::string &scriptJson,
                                                    std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishDialogScript", [&] { return creatures::db->upsertDialogScript(scriptJson, parentSpan); },
        CacheType::DialogScriptList);
}

Result<void> deleteDialogScript(const scriptId_t &scriptId, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "deleteDialogScript", [&] { return creatures::db->deleteDialogScript(scriptId, parentSpan); },
        CacheType::DialogScriptList);
}

Result<creatures::Storyboard> publishStoryboard(const std::string &storyboardJson,
                                                std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishStoryboard", [&] { return creatures::db->upsertStoryboard(storyboardJson, parentSpan); },
        CacheType::StoryboardList);
}

Result<void> deleteStoryboard(const storyboardId_t &storyboardId, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "deleteStoryboard", [&] { return creatures::db->deleteStoryboard(storyboardId, parentSpan); },
        CacheType::StoryboardList);
}

Result<void> deleteAnimation(const animationId_t &animationId, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "deleteAnimation", [&] { return creatures::db->deleteAnimation(animationId, parentSpan); },
        CacheType::Animation);
}

void broadcastCacheInvalidation(CacheType type) {
    // No DB call to pair with — explicit standalone broadcast. The name
    // signals "this is a deliberate manual case" so a reader can tell at a
    // glance it's not a forgotten pairing.
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, type);
}

std::filesystem::path resolveSoundPath(const std::string &stored) {
    if (stored.empty()) {
        return {};
    }
    std::filesystem::path p(stored);
    if (p.is_absolute()) {
        return p;
    }
    // Relative paths resolve under the Permanent root by convention. If config
    // isn't initialized (test contexts) we fall back to the raw relative path
    // — callers that need the file present will fail at open, which is the
    // appropriate failure mode.
    if (!creatures::config) {
        return p;
    }
    return std::filesystem::path(creatures::config->getSoundFileLocation()) / p;
}

} // namespace creatures::storage
