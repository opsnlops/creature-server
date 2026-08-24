
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "api/JsonResponse.h"
#include "api/SoundResponses.h"
#include "model/Sound.h"

namespace creatures {
class OperationSpan;
class RequestSpan;
} // namespace creatures

namespace creatures ::ws {

class SoundService {

  public:
    SoundService() = default;
    virtual ~SoundService() = default;

    /// A resolved sound plus which store it came from. The permanent store is
    /// content-addressed and immutable; the ad-hoc store reuses basenames, so
    /// callers use `fromPermanentStore` to decide cacheability (#57 review).
    struct ResolvedSound {
        std::string path;
        bool fromPermanentStore;
    };

    /// Resolve a sound by basename: permanent store first (recursive basename
    /// search so dialog/ renders resolve — #46), then the ad-hoc store. Returns
    /// std::nullopt if neither has it. The single owner of the store-precedence
    /// policy the rendition/provenance/metadata endpoints all share.
    Result<std::optional<ResolvedSound>> resolveSoundPath(const std::string &filename,
                                                          std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<std::optional<ResolvedSound>> resolveSoundPath(const std::string &filename,
                                                          std::shared_ptr<OperationSpan> parentSpan);

    /// Build the full (heavy) structured metadata for one already-resolved
    /// sound file: size, sidecars, and all embedded iXML metadata including the
    /// per-track mouth cues and word timings. Backs GET /sound/{filename}/metadata
    /// (issue #56) — the sound LIST stays light and omits the heavy arrays.
    Result<Sound> buildSoundMetadata(const std::string &absolutePath, const std::string &filename,
                                     std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Play a sound file for testing
     *
     * @param soundFile
     * @return
     */
    Result<api::StatusResponse> playSound(const std::string &soundFile,
                                          std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Get all of the sound files
     */
    Result<std::vector<Sound>> getAllSounds(std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Get all ad-hoc generated sound files.
     */
    Result<std::vector<api::AdHocSoundEntry>> getAdHocSounds(std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Resolve the absolute path for an ad-hoc sound filename.
     */
    Result<std::string> resolveAdHocSoundPath(const std::string &filename,
                                              std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<std::string> resolveAdHocSoundPath(const std::string &filename, std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Resolve the absolute path for a permanent-store sound by basename.
     *
     * Tries a top-level file first, then walks the permanent sound tree so that
     * sounds living in subdirectories (e.g. dialog/ renders) resolve too (#46).
     * Returns NotFound if nothing matches, InvalidData for an unsafe filename.
     */
    Result<std::string> resolvePermanentSoundPath(const std::string &filename,
                                                  std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<std::string> resolvePermanentSoundPath(const std::string &filename,
                                                  std::shared_ptr<OperationSpan> parentSpan);
};

} // namespace creatures::ws
