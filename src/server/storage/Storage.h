#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "model/Animation.h"
#include "model/CacheInvalidation.h"
#include "model/Creature.h"
#include "model/DialogScript.h"
#include "model/DmxFixture.h"
#include "model/Playlist.h"
#include "model/Storyboard.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

// `creatures::storage` is the **single** place that knows where bytes live on
// disk + what cache invalidations a write fires. Closes the gap from issue #11
// where every file-writing handler reinvented both concerns and could (and
// did, once) forget the invalidation pairing.
//
// New file-writing code should go through this facade. Existing direct calls
// to `std::filesystem::temp_directory_path()`, `config->getSoundFileLocation()`,
// or paired-but-separate `scheduleCacheInvalidationEvent` are migration debts.
namespace creatures::storage {

// Four storage buckets — one per real lifecycle that already exists on disk.
// Picking a bucket implicitly picks its cleanup story; callers don't choose
// cleanup independently.
enum class Persistence {
    // The configured permanent sound root (`config->getSoundFileLocation()`).
    // Never auto-cleaned. Sound files referenced from Animation.metadata.sound_file
    // as RELATIVE paths so the deployment can move the root without rewriting
    // the database.
    Permanent,

    // Ad-hoc artifacts that live until the next TTL sweep. Used for animation
    // sound + JSON pairs that back Animations stored in the ad-hoc collection
    // (insertAdHocAnimation). Stored on Animation.metadata.sound_file as an
    // ABSOLUTE path (they're not in Permanent, so a relative-to-Permanent
    // resolution would point at the wrong place).
    AdHoc,

    // Per-job scratch space. Lives only for the duration of one job; the
    // caller is responsible for `TempDirGuard`-style cleanup at job end.
    // Used for intermediate WAVs during lipsync extraction, etc.
    JobScratch,

    // ElevenLabs generation cache. Subdir layout (cacheKey/generationId.{pcm,json})
    // is owned by `DialogCache`; the facade just supplies the root path. Cleaned
    // by a separate cron sweep.
    GenerationCache,
};

// A path with two faces: where to write the bytes, and what to stamp on the
// model record (Animation.metadata.sound_file, etc.) so the read side knows
// how to find it back.
struct StoragePath {
    // Absolute on-disk path for opening + writing.
    std::filesystem::path absolute;

    // What to persist on the model. Permanent → relative-to-permanent-root
    // (so a deployment move doesn't break stored references). All other
    // persistences → absolute, since the read side can't resolve them otherwise.
    std::string forMetadata;
};

// Returns the root directory for a persistence bucket. Ensures the directory
// exists (and any parents). Returns InternalError if mkdir fails.
[[nodiscard]] Result<std::filesystem::path> root(Persistence);

// Compute (don't write) a path under the given bucket. Ensures the parent
// directory exists. `subdir` lets the caller carve out a sub-namespace
// (e.g. per-job dirs under JobScratch, or "dialog/" under Permanent).
//
// Filename is taken verbatim — caller is responsible for any sanitization
// (especially for Permanent, where the file ends up in a user-visible tree).
[[nodiscard]] Result<StoragePath> allocateSoundPath(Persistence persistence, std::string filename,
                                                    std::optional<std::string> subdir = std::nullopt);

// Write bytes to a persistence bucket atomically (`.tmp` + rename) and fire
// the matching `CacheType::*SoundList` invalidation on success. The invalidation
// is non-negotiable — that's the whole point of routing through this function
// instead of writing directly.
//
// Permanent → CacheType::SoundList
// AdHoc     → CacheType::AdHocSoundList
// JobScratch → no invalidation (not visible to clients)
// GenerationCache → no invalidation (server-internal cache)
//
// On any write/rename failure, partial files are cleaned up and no invalidation
// fires.
[[nodiscard]] Result<StoragePath> writeSoundFile(Persistence persistence, std::string filename,
                                                 std::span<const std::uint8_t> bytes,
                                                 std::optional<std::string> subdir = std::nullopt);

// Persist a NEW permanent animation. Upserts the JSON into the animations
// collection and fires `CacheType::Animation` + `CacheType::SoundList` (a new
// permanent animation almost always has new sound bytes attached).
[[nodiscard]] Result<creatures::Animation> publishAnimation(const std::string &animationJson,
                                                            std::shared_ptr<OperationSpan> parentSpan = nullptr);

// Persist an ad-hoc animation (insertAdHocAnimation collection, TTL-cleaned)
// and fire `CacheType::AdHocAnimationList` + `CacheType::AdHocSoundList`.
[[nodiscard]] Result<void> publishAdHocAnimation(const creatures::Animation &animation,
                                                 std::shared_ptr<OperationSpan> parentSpan = nullptr);

// Republish an existing permanent animation — updates an Animation that's
// already in the DB without producing a new sound file. Fires `CacheType::Animation`
// ONLY (not SoundList — the sound reference didn't change). Used by the lip-sync
// handler, which mutates tracks in-place on an existing animation.
[[nodiscard]] Result<creatures::Animation> republishAnimation(const std::string &animationJson,
                                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

// Resolve a stored sound reference (whatever was on Animation.metadata.sound_file)
// to an absolute path for reading. Absolute paths pass through; relative paths
// are joined under the Permanent root. The inverse of the `forMetadata` rule
// in StoragePath.
[[nodiscard]] std::filesystem::path resolveSoundPath(const std::string &stored);

// =============================================================================
// DB-only publishers — each pairs the db->* call with the matching cache
// invalidation so callers can't fire one without the other (issue #11
// expansion: same footgun as the file-storage publishers, just at the
// DB-only mutation layer).
// =============================================================================

// upsertCreature + CacheType::Creature
[[nodiscard]] Result<creatures::Creature> publishCreature(const std::string &creatureJson,
                                                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

// upsertFixture + CacheType::Fixture
[[nodiscard]] Result<creatures::DmxFixture> publishFixture(const std::string &fixtureJson,
                                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

// deleteFixture + CacheType::Fixture
[[nodiscard]] Result<void> deleteFixture(const fixtureId_t &fixtureId,
                                         std::shared_ptr<OperationSpan> parentSpan = nullptr);

// setFixtureUniverse + CacheType::Fixture (universe assignment changes
// fixture state visible to clients; clients refresh fixtures).
[[nodiscard]] Result<void> setFixtureUniverse(const fixtureId_t &fixtureId, std::optional<universe_t> universe,
                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

// upsertPlaylist + CacheType::Playlist
[[nodiscard]] Result<creatures::Playlist> publishPlaylist(const std::string &playlistJson,
                                                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

// upsertDialogScript + CacheType::DialogScriptList
[[nodiscard]] Result<creatures::DialogScript> publishDialogScript(const std::string &scriptJson,
                                                                  std::shared_ptr<OperationSpan> parentSpan = nullptr);

// deleteDialogScript + CacheType::DialogScriptList
[[nodiscard]] Result<void> deleteDialogScript(const scriptId_t &scriptId,
                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

// upsertStoryboard + CacheType::StoryboardList
[[nodiscard]] Result<creatures::Storyboard> publishStoryboard(const std::string &storyboardJson,
                                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

// deleteStoryboard + CacheType::StoryboardList
[[nodiscard]] Result<void> deleteStoryboard(const storyboardId_t &storyboardId,
                                            std::shared_ptr<OperationSpan> parentSpan = nullptr);

// deleteAnimation + CacheType::Animation. (publishAnimation /
// republishAnimation already exist for the write side from the original
// PR #20; this completes the Animation CRUD surface.)
[[nodiscard]] Result<void> deleteAnimation(const animationId_t &animationId,
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

// Explicit standalone broadcast — for the rare cases where the underlying
// mutation happened outside our process (e.g. debug refresh buttons) or
// where a paired publisher doesn't apply. Same wire effect as the
// invalidation fired inside publishX, just named so it's obvious this is
// a deliberate manual case rather than a forgotten pairing.
void broadcastCacheInvalidation(CacheType type);

} // namespace creatures::storage
