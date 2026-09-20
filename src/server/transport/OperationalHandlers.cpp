#include "server/transport/OperationalHandlers.h"

#include <cstdint>
#include <memory>

#include <fmt/format.h>

#include "api/DebugResponses.h"
#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "model/PlaylistStatus.h"
#include "server/audio/SoundPathResolver.h"
#include "server/config.h"
#include "server/jobs/JobManager.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/storage/Storage.h"
#include "util/AudioCache.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"

namespace creatures {
extern std::shared_ptr<audio::SoundStoreIndex> permanentSoundIndex;
extern std::shared_ptr<audio::SoundStoreIndex> adHocSoundIndex;
extern std::shared_ptr<util::AudioCache> audioCache;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::transport {
namespace {

PreparedResponse status(const int code, std::string message, const std::shared_ptr<OperationSpan> &span,
                        const char *errorType = nullptr) {
    if (span) {
        if (code >= 400) {
            span->setAttribute("error.type", errorType == nullptr ? "HttpError" : errorType);
            span->setAttribute("error.code", static_cast<int64_t>(code));
            span->setError(message);
        } else {
            span->setSuccess();
        }
    }
    return PreparedResponse::json(
        code, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(code, std::move(message)))));
}

std::pair<std::string, const char *> cacheDescription(const CacheType type) {
    switch (type) {
    case CacheType::Creature:
        return {"Creature", "creature"};
    case CacheType::Animation:
        return {"Animation", "animation"};
    case CacheType::Playlist:
        return {"Playlist", "playlist"};
    case CacheType::Fixture:
        return {"Fixture", "fixture"};
    case CacheType::DialogScriptList:
        return {"Dialog script list", "dialog_script_list"};
    case CacheType::StoryboardList:
        return {"Storyboard list", "storyboard_list"};
    case CacheType::StageList:
        return {"Stage list", "stage_list"};
    case CacheType::SoundList:
        return {"Sound list", "sound_list"};
    case CacheType::AdHocAnimationList:
        return {"Ad-hoc animation list", "ad_hoc_animation_list"};
    case CacheType::AdHocSoundList:
        return {"Ad-hoc sound list", "ad_hoc_sound_list"};
    case CacheType::AdHocExchangeList:
        return {"Ad-hoc exchange list", "ad_hoc_exchange_list"};
    case CacheType::Unknown:
        return {"Unknown", "unknown"};
    }
    return {"Unknown", "unknown"};
}

} // namespace

PreparedResponse getJob(const std::string &jobId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(jobId)) {
        return status(400, "jobId must be a UUID", span, "InvalidJobId");
    }
    if (span) {
        span->setAttribute("job.id", canonicalUuid(jobId));
    }
    if (!creatures::jobManager) {
        return status(500, "Job manager unavailable", span, "JobManagerUnavailable");
    }
    const auto job = creatures::jobManager->getJob(jobId);
    if (!job.has_value()) {
        return status(404, fmt::format("job '{}' not found", jobId), span, "JobNotFound");
    }
    const api::JobStateResponse response{job->jobId,
                                         creatures::jobs::toString(job->jobType),
                                         creatures::jobs::toString(job->status),
                                         job->progress,
                                         job->result,
                                         job->details};
    if (span) {
        span->setAttribute("job.type", creatures::jobs::toString(job->jobType));
        span->setAttribute("job.status", creatures::jobs::toString(job->status));
        span->setAttribute("job.progress_percent", static_cast<int64_t>(job->progress * 100.0F));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::jobStateResponseToJson(response)));
}

PreparedResponse invalidateCache(const CacheType type, const std::shared_ptr<OperationSpan> &span) {
    if (type == CacheType::SoundList) {
        creatures::rtp::AudioStreamBuffer::clearMemo();
    }
    creatures::storage::broadcastCacheInvalidation(type);
    if (type == CacheType::SoundList && creatures::permanentSoundIndex) {
        creatures::permanentSoundIndex->rebuildNow();
    } else if (type == CacheType::AdHocSoundList && creatures::adHocSoundIndex) {
        creatures::adHocSoundIndex->rebuildNow();
    }
    const auto [displayName, attributeName] = cacheDescription(type);
    if (span) {
        span->setAttribute("cache.type", attributeName);
    }
    return status(200,
                  fmt::format("{} cache invalidation scheduled for {} frames from now", displayName,
                              CACHE_INVALIDATION_DELAY_TIME),
                  span);
}

PreparedResponse pruneAudioCache(const std::string &dryRun, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::audioCache) {
        return status(500, "Audio cache unavailable", span, "AudioCacheUnavailable");
    }
    bool isDryRun = true;
    if (!dryRun.empty()) {
        if (dryRun == "true" || dryRun == "1") {
            isDryRun = true;
        } else if (dryRun == "false" || dryRun == "0") {
            isDryRun = false;
        } else {
            return status(400, "dry_run must be true or false", span, "InvalidDryRun");
        }
    }
    auto pruneSpan = creatures::observability
                         ? creatures::observability->createChildOperationSpan("DebugController.pruneAudioCache", span)
                         : nullptr;
    const auto result = creatures::audioCache->pruneOrphanedEntries(isDryRun, pruneSpan);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        recordSpanError(pruneSpan, error.getMessage(), "AudioCachePruneFailure", error.getCode());
        return status(serverErrorToStatusCode(error.getCode()), error.getMessage(), span, "AudioCachePruneFailure");
    }
    const auto report = result.getValue().value();
    if (!isDryRun && (report.orphanedEntries > 0 || report.incompleteEntries > 0)) {
        creatures::rtp::AudioStreamBuffer::clearMemo();
    }
    const api::AudioCachePruneResponse response{
        report.dryRun,         report.entriesScanned,    report.orphanedEntries, report.incompleteEntries,
        report.temporaryFiles, report.orphanedLockFiles, report.bytesReclaimed,  report.removed};
    if (pruneSpan) {
        pruneSpan->setAttribute("cache.prune.dry_run", report.dryRun);
        pruneSpan->setAttribute("cache.entries_scanned", static_cast<int64_t>(report.entriesScanned));
        pruneSpan->setAttribute("cache.removed.count", static_cast<int64_t>(report.removed.size()));
        pruneSpan->setAttribute("cache.bytes_reclaimed", static_cast<int64_t>(report.bytesReclaimed));
        pruneSpan->setSuccess();
    }
    if (span) {
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::audioCachePruneResponseToJson(response)));
}

PreparedResponse sendDebugPlaylistUpdate(const std::shared_ptr<OperationSpan> &span) {
    PlaylistStatus playlistStatus{};
    playlistStatus.universe = 42;
    playlistStatus.playlist = "4b5aa09e-9a61-47e7-86d2-3d8f59ebd9a7";
    playlistStatus.playing = true;
    playlistStatus.current_animation = "2241e872-57b3-4fa3-8e76-1c2f517f998d";
    broadcastPlaylistStatusToAllClients(playlistStatus);
    return status(200, "Playlist update sent", span);
}

} // namespace creatures::transport
