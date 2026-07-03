
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"
#include "server/ws/dto/DialogDto.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures ::ws {

/// HTTP-free service holding the dialog-preview generation/assembly pipeline.
///
/// Both the sync cache-read path (DialogPreviewController) and the async
/// generation path (JobWorker) call one implementation here. Nothing in this
/// class touches oatpp HTTP responses — errors come back as Result<T> and the
/// callers decide how to surface them (bailHttp on the controller, failJob on
/// the worker).
class DialogPreviewService {
  public:
    DialogPreviewService() = default;
    virtual ~DialogPreviewService() = default;

    /// Per-creature info the preview needs from the DB. Mono-format previews
    /// only need voiceId; multichannel additionally needs audioChannel.
    struct PreviewCreature {
        std::string creatureId;
        std::string voiceId;
        uint16_t audioChannel; // 1-based; used only for multichannel export
    };

    /// Everything an endpoint/worker needs after the "resolve creatures →
    /// find or create a generation" pipeline has run successfully.
    struct PreviewOutcome {
        creatures::voice::CachedGeneration generation;
        std::string cacheKey;
        bool cached = false;
        std::vector<creatures::voice::DialogInput> inputs;
        std::unordered_map<std::string, PreviewCreature> resolved;
    };

    /// Result of the cheap fast-path cache probe used by the meta controller.
    /// `cacheHit == true` means `outcome` is populated and can be served
    /// synchronously; `false` means a generation job should be created.
    struct MetaFastPath {
        bool cacheHit = false;
        std::optional<PreviewOutcome> outcome;
        std::string cacheKey;
    };

    /// Cheap, HTTP-free fast path for the meta endpoint. Resolves creatures,
    /// computes the cache key, and — if the request named a generation_id, or
    /// regenerate is false and a latest take exists — loads it. Does NO
    /// ElevenLabs work: on a miss it returns `cacheHit == false` so the caller
    /// can enqueue a job instead. Errors (bad creature, explicit generation_id
    /// not cached) come back as ServerError.
    creatures::Result<MetaFastPath> tryServeFromCache(const oatpp::Object<DialogPreviewRequestDto> &body,
                                                      const std::shared_ptr<creatures::OperationSpan> &opSpan,
                                                      const char *spanAttrName);

    /// Full "resolve creatures → check cache → maybe call ElevenLabs" path.
    /// Always yields a usable generation (generating fresh when there's no
    /// cache hit or regenerate was requested). `progress`, when set, is invoked
    /// once per generated chunk with a 0..1 fraction.
    /// `jobId`, when non-empty, is stamped on the per-chunk spans so a job's chunk
    /// work can be pivoted to across traces in Honeycomb by `job.id`.
    creatures::Result<PreviewOutcome> loadOrGenerate(const oatpp::Object<DialogPreviewRequestDto> &body,
                                                     const std::shared_ptr<creatures::OperationSpan> &opSpan,
                                                     const char *spanAttrName,
                                                     std::function<void(float)> progress = nullptr,
                                                     const std::string &jobId = "");

    /// Assemble the 17-channel WAV for a resolved outcome and write it to
    /// `wavPath`. Validates the creature → audio_channel mapping (distinct, in
    /// [1,17]) and runs the slice + interleave. HTTP-free: returns ServerError
    /// on any failure.
    creatures::Result<void> exportMultichannel(const PreviewOutcome &outcome, const std::filesystem::path &wavPath,
                                               const std::shared_ptr<creatures::OperationSpan> &opSpan);

    /// Pack our internal voice_segments / forced_alignment into the response DTO.
    static void populateMetaResponse(oatpp::Object<DialogPreviewMetaResponseDto> &dto,
                                     const creatures::voice::CachedGeneration &gen, const std::string &cacheKey,
                                     bool cached);

    /// Walk the request's turns, resolve each unique creature_id to its
    /// voice_id (+ audio_channel for multichannel use). Shared with the
    /// controller's lookup endpoint.
    static creatures::Result<std::unordered_map<std::string, PreviewCreature>>
    resolveCreatures(const oatpp::List<oatpp::Object<DialogTurnDto>> &turns,
                     const std::shared_ptr<creatures::OperationSpan> &span);

    /// Convert the API DTO turns + resolved creature lookup into the internal
    /// DialogInput list (voice_id + text).
    static std::vector<creatures::voice::DialogInput>
    buildDialogInputs(const oatpp::List<oatpp::Object<DialogTurnDto>> &turns,
                      const std::unordered_map<std::string, PreviewCreature> &resolved);

  private:
    /// Shared "resolve creatures → compute cache key → try cache" probe. On a
    /// hit, `generation` is set. Callers decide whether a miss means "generate"
    /// (loadOrGenerate) or "enqueue a job" (tryServeFromCache).
    struct CacheProbe {
        std::string cacheKey;
        std::vector<creatures::voice::DialogInput> inputs;
        std::unordered_map<std::string, PreviewCreature> resolved;
        std::optional<creatures::voice::CachedGeneration> generation;
        bool cached = false;
        bool regenerate = false;
    };
    creatures::Result<CacheProbe> probeCache(const oatpp::Object<DialogPreviewRequestDto> &body,
                                             const std::shared_ptr<creatures::OperationSpan> &opSpan,
                                             const char *spanAttrName);

    /// Build a short summary for cache metadata (first ~80 chars of joined turn
    /// text). Human-readable debugging when ls'ing the cache dir.
    static std::string makeTurnsSummary(const std::vector<creatures::voice::DialogInput> &inputs);
};

} // namespace creatures::ws
