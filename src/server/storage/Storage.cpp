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

Result<creatures::Stage> publishStage(const std::string &stageJson, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "publishStage", [&] { return creatures::db->upsertStage(stageJson, parentSpan); }, CacheType::StageList);
}

Result<void> deleteStage(const stageId_t &stageId, std::shared_ptr<OperationSpan> parentSpan) {
    return runPublisher(
        "deleteStage", [&] { return creatures::db->deleteStage(stageId, parentSpan); }, CacheType::StageList);
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

Result<void> deleteSupersededDialogSound(const std::string &stored, std::shared_ptr<OperationSpan> parentSpan) {
    (void)parentSpan; // no DB call to trace here; the caller's span covers it

    auto declineQuietly = [&](const char *reason) {
        debug("not deleting superseded sound '{}': {}", stored, reason);
        return Result<void>{};
    };

    if (stored.empty()) {
        return declineQuietly("empty reference");
    }

    // Only this pipeline's own generated audio is ever a candidate. Sounds a
    // human uploaded live at the root of the sound tree, not under dialog/.
    if (stored.rfind("dialog/", 0) != 0) {
        return declineQuietly("not under dialog/");
    }
    if (!creatures::config) {
        return declineQuietly("no configuration available to resolve the sound root");
    }

    std::error_code ec;
    const auto root =
        std::filesystem::weakly_canonical(std::filesystem::path(creatures::config->getSoundFileLocation()), ec);
    if (ec) {
        return declineQuietly("sound root could not be resolved");
    }
    const auto target = std::filesystem::weakly_canonical(resolveSoundPath(stored), ec);
    if (ec) {
        return declineQuietly("path could not be resolved");
    }

    // sound_file arrives from a stored Animation, which is client-writable via
    // the animation upsert. Containment is checked on the CANONICAL paths so a
    // reference like dialog/../../etc/passwd cannot escape the tree.
    const auto rootStr = root.string();
    const auto targetStr = target.string();
    if (targetStr.size() <= rootStr.size() || targetStr.compare(0, rootStr.size(), rootStr) != 0 ||
        targetStr[rootStr.size()] != std::filesystem::path::preferred_separator) {
        warn("refusing to delete sound '{}': resolves to '{}', outside the sound root '{}'", stored, targetStr,
             rootStr);
        return Result<void>{};
    }

    if (!std::filesystem::exists(target, ec) || ec) {
        return declineQuietly("file does not exist");
    }

    const auto size = std::filesystem::file_size(target, ec);
    std::filesystem::remove(target, ec);
    if (ec) {
        // A failed cleanup is not a failed render. Say so and move on.
        warn("could not delete superseded sound '{}': {}", targetStr, ec.message());
        return Result<void>{};
    }

    info("deleted superseded dialog sound '{}' ({:.1f} MB)", stored, static_cast<double>(size) / 1e6);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::SoundList);
    return Result<void>{};
}

namespace {

constexpr const char *kVoiceTakeSubdir = "dialog/voice";
constexpr const char *kAdHocExportSubdir = "preview-exports";

/// Move a file between buckets. rename() is the fast path; buckets can sit on
/// different filesystems, so fall back to copy + remove rather than failing.
Result<void> moveFile(const std::filesystem::path &from, const std::filesystem::path &to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    std::filesystem::rename(from, to, ec);
    if (!ec) {
        return Result<void>{};
    }
    ec.clear();
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Result<void>{
            ServerError(ServerError::InternalError,
                        fmt::format("could not move '{}' to '{}': {}", from.string(), to.string(), ec.message()))};
    }
    std::filesystem::remove(from, ec);
    if (ec) {
        // The copy landed, so the move succeeded from the caller's point of
        // view; the leftover is a tidiness problem, not a correctness one.
        warn("moved '{}' to '{}' but could not remove the original: {}", from.string(), to.string(), ec.message());
    }
    return Result<void>{};
}

} // namespace

std::string voiceTakeAdHocFilename(const std::string &generationId) {
    return fmt::format("dialog-17ch-{}.wav", generationId);
}

Result<std::filesystem::path> voiceTakeAdHocPath(const std::string &generationId) {
    auto adHocRoot = root(Persistence::AdHoc);
    if (!adHocRoot.isSuccess()) {
        return Result<std::filesystem::path>{adHocRoot.getError().value()};
    }
    return Result<std::filesystem::path>{adHocRoot.getValue().value() / kAdHocExportSubdir /
                                         voiceTakeAdHocFilename(generationId)};
}

Result<StoragePath> promoteVoiceTake(const std::string &generationId, std::string filename,
                                     std::shared_ptr<OperationSpan> parentSpan) {
    (void)parentSpan;

    auto sourceResult = voiceTakeAdHocPath(generationId);
    if (!sourceResult.isSuccess()) {
        return Result<StoragePath>{sourceResult.getError().value()};
    }
    const auto source = sourceResult.getValue().value();

    std::error_code ec;
    if (!std::filesystem::exists(source, ec) || ec) {
        // Callers are expected to have assembled the export first; reaching
        // here means it couldn't be built and couldn't be found. Don't blame
        // the TTL — a take generated seconds ago hits this too when its
        // 17-channel WAV was never written.
        return Result<StoragePath>{
            ServerError(ServerError::NotFound,
                        fmt::format("no ad-hoc audio exists for take {} — it was never exported to a 17-channel "
                                    "WAV, or the ad-hoc sweep has already reclaimed it",
                                    generationId))};
    }

    auto target = allocateSoundPath(Persistence::Permanent, std::move(filename), std::string(kVoiceTakeSubdir));
    if (!target.isSuccess()) {
        return Result<StoragePath>{target.getError().value()};
    }
    const auto destination = target.getValue().value();

    auto moved = moveFile(source, destination.absolute);
    if (!moved.isSuccess()) {
        return Result<StoragePath>{moved.getError().value()};
    }

    info("promoted voice take {} to '{}'", generationId, destination.forMetadata);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::SoundList);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocSoundList);
    return Result<StoragePath>{destination};
}

Result<void> demoteVoiceTake(const std::string &stored, const std::string &generationId,
                             std::shared_ptr<OperationSpan> parentSpan) {
    (void)parentSpan;

    auto declineQuietly = [&](const char *reason) {
        debug("not demoting voice take '{}': {}", stored, reason);
        return Result<void>{};
    };

    if (stored.empty()) {
        return declineQuietly("empty reference");
    }
    // Only ever touch this pipeline's own promoted takes.
    if (stored.rfind(std::string(kVoiceTakeSubdir) + "/", 0) != 0) {
        return declineQuietly("not under dialog/voice/");
    }
    if (!creatures::config) {
        return declineQuietly("no configuration available to resolve the sound root");
    }

    std::error_code ec;
    const auto permanentRoot =
        std::filesystem::weakly_canonical(std::filesystem::path(creatures::config->getSoundFileLocation()), ec);
    if (ec) {
        return declineQuietly("sound root could not be resolved");
    }
    const auto source = std::filesystem::weakly_canonical(resolveSoundPath(stored), ec);
    if (ec) {
        return declineQuietly("path could not be resolved");
    }

    // sound_file is client-writable through the script upsert, so containment
    // is checked on the canonical paths — same guard as #130.
    const auto rootStr = permanentRoot.string();
    const auto srcStr = source.string();
    if (srcStr.size() <= rootStr.size() || srcStr.compare(0, rootStr.size(), rootStr) != 0 ||
        srcStr[rootStr.size()] != std::filesystem::path::preferred_separator) {
        warn("refusing to demote voice take '{}': resolves outside the sound root", stored);
        return Result<void>{};
    }
    if (!std::filesystem::exists(source, ec) || ec) {
        return declineQuietly("file does not exist");
    }

    auto destinationResult = voiceTakeAdHocPath(generationId);
    if (!destinationResult.isSuccess()) {
        warn("could not resolve the ad-hoc root to demote '{}'; leaving it in place", stored);
        return Result<void>{};
    }
    const auto destination = destinationResult.getValue().value();

    auto moved = moveFile(source, destination);
    if (!moved.isSuccess()) {
        // Demotion failing must never fail the acceptance that triggered it.
        warn("could not demote voice take '{}': {}", stored, moved.getError().value().getMessage());
        return Result<void>{};
    }

    info("demoted voice take '{}' back to ad-hoc (TTL restarted)", stored);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::SoundList);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocSoundList);
    return Result<void>{};
}

} // namespace creatures::storage
