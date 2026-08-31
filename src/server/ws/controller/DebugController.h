
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/DebugResponses.h"
#include "server/audio/SoundPathResolver.h"
#include "server/config.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/storage/Storage.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "util/AudioCache.h"
#include "util/ObservabilityManager.h"

#include "util/websocketUtils.h"

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures {
extern std::shared_ptr<creatures::audio::SoundStoreIndex> permanentSoundIndex;
extern std::shared_ptr<creatures::audio::SoundStoreIndex> adHocSoundIndex;
extern std::shared_ptr<creatures::util::AudioCache> audioCache;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures ::ws {

class DebugController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<DebugController> {
  public:
    DebugController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    // MetricsService m_metricsService;
  public:
    static std::shared_ptr<DebugController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                 objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<DebugController>(objectMapper);
    }

    ENDPOINT_INFO(invalidate_creature) {
        info->summary = "Sends a message to all clients to invalidate their creature cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/creature", invalidate_creature,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/debug/cache-invalidate/creature", "GET", "api/v1/debug/cache-invalidate/creature",
            "invalidate_creature", "DebugController", request, [&](const auto &span) {
                creatures::storage::broadcastCacheInvalidation(CacheType::Creature);
                auto statusMessage = fmt::format("Creature cache invalidation scheduled for {} frames from now",
                                                 CACHE_INVALIDATION_DELAY_TIME);
                debug(statusMessage);
                return okStatus(span, Status::CODE_200, statusMessage);
            });
    }

    ENDPOINT_INFO(invalidate_animation) {
        info->summary = "Sends a message to all clients to invalidate their animation cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/animation", invalidate_animation,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/debug/cache-invalidate/animation", "GET", "api/v1/debug/cache-invalidate/animation",
            "invalidate_animation", "DebugController", request, [&](const auto &span) {
                creatures::storage::broadcastCacheInvalidation(CacheType::Animation);
                auto statusMessage = fmt::format("Animation cache invalidation scheduled for {} frames from now",
                                                 CACHE_INVALIDATION_DELAY_TIME);
                debug(statusMessage);
                return okStatus(span, Status::CODE_200, statusMessage);
            });
    }

    ENDPOINT_INFO(invalidate_playlist) {
        info->summary = "Sends a message to all clients to invalidate their playlist cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/playlist", invalidate_playlist,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/debug/cache-invalidate/playlist", "GET", "api/v1/debug/cache-invalidate/playlist",
            "invalidate_playlist", "DebugController", request, [&](const auto &span) {
                creatures::storage::broadcastCacheInvalidation(CacheType::Playlist);
                auto statusMessage = fmt::format("Playlist cache invalidation scheduled for {} frames from now",
                                                 CACHE_INVALIDATION_DELAY_TIME);
                debug(statusMessage);
                return okStatus(span, Status::CODE_200, statusMessage);
            });
    }

    ENDPOINT_INFO(invalidate_fixture) {
        info->summary = "Sends a message to all clients to invalidate their fixture cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/fixture", invalidate_fixture,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/debug/cache-invalidate/fixture", "GET", "api/v1/debug/cache-invalidate/fixture",
                           "invalidate_fixture", "DebugController", request, [&](const auto &span) {
                               creatures::storage::broadcastCacheInvalidation(CacheType::Fixture);
                               auto statusMessage =
                                   fmt::format("Fixture cache invalidation scheduled for {} frames from now",
                                               CACHE_INVALIDATION_DELAY_TIME);
                               debug(statusMessage);
                               return okStatus(span, Status::CODE_200, statusMessage);
                           });
    }

    ENDPOINT_INFO(invalidate_dialog_script_list) {
        info->summary = "Sends a message to all clients to invalidate their dialog-script-list cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/dialog-script-list", invalidate_dialog_script_list,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/debug/cache-invalidate/dialog-script-list", "GET",
                           "api/v1/debug/cache-invalidate/dialog-script-list", "invalidate_dialog_script_list",
                           "DebugController", request, [&](const auto &span) {
                               creatures::storage::broadcastCacheInvalidation(CacheType::DialogScriptList);
                               auto statusMessage =
                                   fmt::format("Dialog script list cache invalidation scheduled for {} frames from now",
                                               CACHE_INVALIDATION_DELAY_TIME);
                               debug(statusMessage);
                               return okStatus(span, Status::CODE_200, statusMessage);
                           });
    }

    ENDPOINT_INFO(invalidate_storyboard_list) {
        info->summary = "Sends a message to all clients to invalidate their storyboard-list cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/storyboard-list", invalidate_storyboard_list,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/debug/cache-invalidate/storyboard-list", "GET",
                           "api/v1/debug/cache-invalidate/storyboard-list", "invalidate_storyboard_list",
                           "DebugController", request, [&](const auto &span) {
                               creatures::storage::broadcastCacheInvalidation(CacheType::StoryboardList);
                               auto statusMessage =
                                   fmt::format("Storyboard list cache invalidation scheduled for {} frames from now",
                                               CACHE_INVALIDATION_DELAY_TIME);
                               debug(statusMessage);
                               return okStatus(span, Status::CODE_200, statusMessage);
                           });
    }

    ENDPOINT_INFO(invalidate_stage_list) {
        info->summary = "Sends a message to all clients to invalidate their stage-list cache";
        info->addTag("Debug");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/stage-list", invalidate_stage_list,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/debug/cache-invalidate/stage-list", "GET", "api/v1/debug/cache-invalidate/stage-list",
            "invalidate_stage_list", "DebugController", request, [&](const auto &span) {
                creatures::storage::broadcastCacheInvalidation(CacheType::StageList);
                auto statusMessage = fmt::format("Stage list cache invalidation scheduled for {} frames from now",
                                                 CACHE_INVALIDATION_DELAY_TIME);
                debug(statusMessage);
                return okStatus(span, Status::CODE_200, statusMessage);
            });
    }

    ENDPOINT_INFO(prune_audio_cache) {
        info->summary = "Reclaim Opus cache entries that can never be used again";
        info->description =
            "The on-disk Opus cache is keyed by a hash of each sound's canonical path, so MOVING or deleting a "
            "sound orphans its cache entry permanently — no other operation reclaims it. This removes orphaned "
            "entries, entries left incomplete by a crashed save, abandoned temporary files, and lock files whose "
            "directory is gone.\n\n"
            "Defaults to a DRY RUN: pass dry_run=false to actually delete. Intended as a creature-cli maintenance "
            "command. Distinct from /cache-invalidate/*, which never deletes anything (issue #166).";
        info->addTag("Debug");
        info->queryParams.add<oatpp::Boolean>("dry_run").required = false;
        info->queryParams["dry_run"].description = "When true (default), report what would be removed without "
                                                   "deleting.";

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/debug/cache/audio/prune", prune_audio_cache,
             QUERY(oatpp::Boolean, dryRun, "dry_run", "true"), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/debug/cache/audio/prune", "POST", "api/v1/debug/cache/audio/prune", "prune_audio_cache",
            "DebugController", request, [&](const auto &span) {
                if (!creatures::audioCache)
                    return bailHttp(span, Status::CODE_500, "Audio cache unavailable");
                const bool isDryRun = dryRun == nullptr || *dryRun;

                auto pruneSpan =
                    creatures::observability
                        ? creatures::observability->createOperationSpan("DebugController.pruneAudioCache", span)
                        : nullptr;
                auto pruneResult = creatures::audioCache->pruneOrphanedEntries(isDryRun, pruneSpan);
                if (!pruneResult.isSuccess()) {
                    const auto error = pruneResult.getError().value();
                    recordSpanError(pruneSpan, error.getMessage(), "AudioCachePruneFailure", error.getCode());
                    return bailFromServerError(span, error);
                }
                const auto report = pruneResult.getValue().value();

                // Anything we removed may also be memoized in RAM; drop those
                // buffers so a later play cannot serve a buffer whose disk
                // cache we just deleted.
                if (!isDryRun && (report.orphanedEntries > 0 || report.incompleteEntries > 0)) {
                    creatures::rtp::AudioStreamBuffer::clearMemo();
                }

                const api::AudioCachePruneResponse response{
                    report.dryRun,         report.entriesScanned,    report.orphanedEntries, report.incompleteEntries,
                    report.temporaryFiles, report.orphanedLockFiles, report.bytesReclaimed,  report.removed};

                info("Audio cache prune via API ({}): {} orphaned, {} incomplete, {} bytes",
                     isDryRun ? "dry run" : "applied", report.orphanedEntries, report.incompleteEntries,
                     report.bytesReclaimed);
                if (pruneSpan) {
                    pruneSpan->setAttribute("cache.prune.dry_run", report.dryRun);
                    pruneSpan->setAttribute("cache.entries_scanned", static_cast<int64_t>(report.entriesScanned));
                    pruneSpan->setAttribute("cache.orphaned_entries", static_cast<int64_t>(report.orphanedEntries));
                    pruneSpan->setAttribute("cache.incomplete_entries", static_cast<int64_t>(report.incompleteEntries));
                    pruneSpan->setAttribute("cache.temporary_files", static_cast<int64_t>(report.temporaryFiles));
                    pruneSpan->setAttribute("cache.orphaned_lock_files",
                                            static_cast<int64_t>(report.orphanedLockFiles));
                    pruneSpan->setAttribute("cache.removed.count", static_cast<int64_t>(report.removed.size()));
                    pruneSpan->setAttribute("cache.bytes_reclaimed", static_cast<int64_t>(report.bytesReclaimed));
                    pruneSpan->setSuccess();
                }
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::audioCachePruneResponseToJson(response));
            });
    }

    ENDPOINT_INFO(invalidate_sound_list) {
        info->summary = "Sends a message to all clients to invalidate their sound-list cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/sound-list", invalidate_sound_list,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/debug/cache-invalidate/sound-list", "GET", "api/v1/debug/cache-invalidate/sound-list",
            "invalidate_sound_list", "DebugController", request, [&](const auto &span) {
                // Also the operator's lever for the one staleness case the
                // in-memory audio memo cannot detect on its own: a sound file
                // replaced out-of-band with the same size AND mtime
                // (issue #93).
                creatures::rtp::AudioStreamBuffer::clearMemo();
                // Broadcast first (it marks the index dirty), THEN rebuild —
                // the reverse order threw the synchronous walk away and paid
                // for a second one on the next lookup (issue #94 review).
                creatures::storage::broadcastCacheInvalidation(CacheType::SoundList);
                if (creatures::permanentSoundIndex) {
                    creatures::permanentSoundIndex->rebuildNow();
                }
                auto statusMessage = fmt::format("Sound list cache invalidation scheduled for {} frames from now",
                                                 CACHE_INVALIDATION_DELAY_TIME);
                debug(statusMessage);
                return okStatus(span, Status::CODE_200, statusMessage);
            });
    }

    ENDPOINT_INFO(invalidate_adhoc_animation_list) {
        info->summary = "Sends a message to all clients to invalidate their ad-hoc-animation-list cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/ad-hoc-animation-list", invalidate_adhoc_animation_list,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/debug/cache-invalidate/ad-hoc-animation-list", "GET",
                           "api/v1/debug/cache-invalidate/ad-hoc-animation-list", "invalidate_adhoc_animation_list",
                           "DebugController", request, [&](const auto &span) {
                               creatures::storage::broadcastCacheInvalidation(CacheType::AdHocAnimationList);
                               auto statusMessage = fmt::format(
                                   "Ad-hoc animation list cache invalidation scheduled for {} frames from now",
                                   CACHE_INVALIDATION_DELAY_TIME);
                               debug(statusMessage);
                               return okStatus(span, Status::CODE_200, statusMessage);
                           });
    }

    ENDPOINT_INFO(invalidate_adhoc_sound_list) {
        info->summary = "Sends a message to all clients to invalidate their ad-hoc-sound-list cache";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/ad-hoc-sound-list", invalidate_adhoc_sound_list,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/debug/cache-invalidate/ad-hoc-sound-list", "GET",
                           "api/v1/debug/cache-invalidate/ad-hoc-sound-list", "invalidate_adhoc_sound_list",
                           "DebugController", request, [&](const auto &span) {
                               creatures::storage::broadcastCacheInvalidation(CacheType::AdHocSoundList);
                               if (creatures::adHocSoundIndex) {
                                   creatures::adHocSoundIndex->rebuildNow();
                               }
                               auto statusMessage =
                                   fmt::format("Ad-hoc sound list cache invalidation scheduled for {} frames from now",
                                               CACHE_INVALIDATION_DELAY_TIME);
                               debug(statusMessage);
                               return okStatus(span, Status::CODE_200, statusMessage);
                           });
    }

    ENDPOINT_INFO(test_playlist_updates) {
        info->summary = "Tests the ability to send a playlist update";
        info->addTag("Debug");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/playlist/update", test_playlist_updates,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/debug/playlist/update", "GET", "api/v1/debug/playlist/update",
                           "test_playlist_updates", "DebugController", request, [&](const auto &span) {
                               PlaylistStatus playlistStatus{};
                               playlistStatus.universe = 42;
                               playlistStatus.playlist = "4b5aa09e-9a61-47e7-86d2-3d8f59ebd9a7";
                               playlistStatus.playing = true;
                               playlistStatus.current_animation = "2241e872-57b3-4fa3-8e76-1c2f517f998d";
                               broadcastPlaylistStatusToAllClients(playlistStatus);
                               return okStatus(span, Status::CODE_200, "Playlist update sent");
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen
