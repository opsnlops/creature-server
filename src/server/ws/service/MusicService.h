#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/DialogContracts.h"
#include "api/MusicContracts.h"
#include "model/MusicPiece.h"
#include "server/storage/Storage.h"
#include "server/voice/IxmlWriter.h"
#include "server/voice/MusicClient.h"
#include "server/voice/MusicGenerationCache.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

/// What a caller knows about a take before ElevenLabs is called: identity,
/// where it came from, and what to record. The dialog path fills the
/// `sourceDialog*` fields; the library path fills `sections` / `pieceId`.
struct MusicComposeContext {
    std::string generationId; // minted by the caller so the upstream span carries it
    std::string title;        // script title or piece title; may be empty
    std::string scriptId;     // empty for library work
    std::vector<voice::DialogScriptLine> scriptLines;
    std::string sourceDialogGenerationId;
    std::string sourceDialogCacheKey;
    int64_t sourceScriptUpdatedAt{0};
    int64_t sourceDialogDurationMs{0};
    int64_t durationExtensionMs{0};
    std::optional<std::vector<voice::MusicSection>> sections;
    std::string pieceId;
    std::string baseVersionId;
    std::string mp3UrlPrefix; // ".../generated/" — the route the caller serves candidates on
};

/// Music generation shared by the dialog-bound and library paths (#200/#202):
/// the ElevenLabs call, provenance, and the candidate-cache write. Callers
/// decide the length and the binding; this decides how a take is recorded.
class MusicService {
  public:
    /// Call ElevenLabs with `upstream`, record provenance from `context`, and
    /// save the candidate. The result is the #200 result shape.
    Result<api::DialogMusicGenerationResult> compose(const voice::MusicGenerationRequest &upstream,
                                                     const MusicComposeContext &context,
                                                     voice::MusicGenerationTraceContext traceContext,
                                                     const std::shared_ptr<OperationSpan> &span) const;

    /// Library generation (#202): prompt, plan, or sections mode; no dialog.
    Result<api::DialogMusicGenerationResult> generate(const api::MusicGenerateRequest &request,
                                                      std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                                      const std::string &jobId = "") const;

    /// Promote a candidate into the library as a new piece or a new version.
    /// `created` is set when a piece was created (201) rather than extended.
    Result<MusicPiece> save(const std::string &generationId, const api::MusicSaveRequest &request, bool &created,
                            SpanParent parentSpan = nullptr) const;

    /// The instruction box (#202): ask ElevenLabs' planner to edit a
    /// version's sections per `instruction`, and report which sections it
    /// changed. Synchronous; no audio is generated.
    Result<api::MusicRefineResult> refine(const std::string &pieceId, const api::MusicRefineRequest &request,
                                          SpanParent parentSpan = nullptr) const;

    /// Dialog-free plan draft from a prompt, normalised to editable sections.
    Result<api::MusicPlanResult> plan(const api::MusicPlanRequest &request, SpanParent parentSpan = nullptr) const;

    Result<std::vector<MusicPiece>> list(SpanParent parentSpan = nullptr) const;
    Result<MusicPiece> get(const std::string &pieceId, SpanParent parentSpan = nullptr) const;
    Result<MusicPiece> update(const std::string &pieceId, const api::MusicPieceUpdateRequest &request,
                              SpanParent parentSpan = nullptr) const;
    Result<void> remove(const std::string &pieceId, SpanParent parentSpan = nullptr) const;

    /// What generate() sends upstream and records, given the request and the
    /// piece it refines (nullptr for a piece-less request). Pure, so the base
    /// version resolution and the sections builder are testable without
    /// ElevenLabs or Mongo.
    struct Prepared {
        voice::MusicGenerationRequest upstream;
        MusicComposeContext context;
    };
    static Result<Prepared> prepare(const api::MusicGenerateRequest &request, const MusicPiece *piece);

    /// The console-facing recipe, derived from provenance so the job result
    /// and the recipe endpoint can never disagree.
    static api::DialogMusicRecipe recipeFromProvenance(const voice::MusicWavProvenance &music);

    /// Copy a verified candidate WAV into the permanent bucket with
    /// `provenance` embedded, read it back, and verify. Rolls the file back if
    /// the read-back fails. Shared by dialog promotion and library save.
    static Result<storage::StoragePath> publishCandidateWav(const voice::CachedMusicGeneration &candidate,
                                                            const voice::WavProvenance &provenance,
                                                            const std::string &filename, const std::string &subdir,
                                                            const std::shared_ptr<OperationSpan> &span);
};

} // namespace creatures::ws
