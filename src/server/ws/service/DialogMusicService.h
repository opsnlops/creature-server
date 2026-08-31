#pragma once

#include <memory>
#include <string>

#include "api/DialogContracts.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

class DialogMusicService {
  public:
    Result<api::DialogMusicGenerationResult> generate(const api::DialogMusicRequest &request,
                                                      std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                                      const std::string &jobId = "") const;

    Result<api::DialogMusicPromotionResult> promote(const std::string &generationId,
                                                    std::shared_ptr<RequestSpan> parentSpan = nullptr) const;

  private:
    /// Repair an already-accepted music block whose composition source was
    /// never recorded (#136), reading it from the PROMOTED file's embedded
    /// provenance rather than from the generation cache.
    ///
    /// This exists because the candidate is TTL'd while the promoted copy is
    /// permanent: the oldest accepted music — the music that most needs the
    /// backfill — is exactly the music whose generation record is gone. The
    /// script has to be found by scanning for the generation id, because the
    /// record that used to carry `scriptId` is what expired.
    ///
    /// Returns NotFound when no accepted music anywhere claims this
    /// generation, which lets the caller report the original cache miss
    /// instead of a confusing repair failure.
    Result<api::DialogMusicPromotionResult>
    backfillMusicSourceFromPromotedFile(const std::string &generationId,
                                        const std::shared_ptr<OperationSpan> &span) const;
};

} // namespace creatures::ws
