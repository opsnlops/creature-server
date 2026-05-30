#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "DialogClient.h"
#include "util/Result.h"

namespace creatures::voice {

/// One captured "take" of an ElevenLabs Text-to-Dialogue + Forced-Alignment
/// call, persisted to disk so subsequent previews / full-pipeline runs can
/// reuse it without re-paying the API cost.
///
/// A `cache_key` (sha256 of the turns) groups multiple generations of the
/// same input; a `generation_id` (UUID) addresses one specific take. The
/// generation captures everything the worker would otherwise have to
/// re-fetch from ElevenLabs:
///   - the mixed-mono PCM (mono S16 @ 48 kHz),
///   - the voice_segments (speaker → char-range mapping),
///   - the forced-alignment result (real per-character timing).
///
/// Stored on disk under the ad-hoc temp root so the existing cron sweep
/// cleans it up — no in-process TTL/LRU bookkeeping needed.
struct CachedGeneration {
    std::string generationId;
    /// Raw mono S16 PCM @ 48 kHz, exactly what generateDialog(pcm_48000)
    /// returned. The downstream pipeline operates on this verbatim.
    std::vector<uint8_t> audioPcm;
    /// Speaker → char-range mapping from text-to-dialogue. The TIMES inside
    /// each segment are unreliable on eleven_v3 (kept for diagnostics only);
    /// the character INDICES are reliable.
    std::vector<DialogVoiceSegment> voiceSegments;
    /// Real per-character / per-word timing for the audio + tag-stripped
    /// transcript. The cache stores it so a re-run doesn't have to call
    /// forced-alignment either.
    ForcedAlignmentResult forcedAlignment;
    /// When this generation was first created.
    std::chrono::system_clock::time_point createdAt;
    /// Short human-readable summary of the input — e.g. first ~80 chars of
    /// the concatenated turn text — for debugging directory listings.
    std::string turnsSummary;
};

/// Index entry returned by listGenerations.
struct GenerationListEntry {
    std::string generationId;
    std::chrono::system_clock::time_point createdAt;
};

/// Compute the stable cache key for a list of turns: sha256(JSON([{v,t}, ...])).
///
/// Stable across processes — the JSON serialization is deterministic for the
/// known schema. Returns 64-char lowercase hex.
std::string computeCacheKey(const std::vector<DialogInput> &turns);

/// Return every cached generation for `cacheKey`, newest first (by file mtime
/// — `created_at` in the metadata is preserved separately for debugging but
/// mtime is what we sort on for cache freshness). Empty list if nothing cached.
std::vector<GenerationListEntry> listGenerations(const std::string &cacheKey);

/// Convenience: the most recent generation_id for cacheKey, or nullopt if
/// nothing cached. Equivalent to listGenerations(cacheKey).front().generationId
/// when non-empty.
std::optional<std::string> findLatestGeneration(const std::string &cacheKey);

/// Read a specific cached generation. Returns NotFound if either the .pcm or
/// .json file is missing or unreadable, InvalidData if the .json is malformed.
Result<CachedGeneration> loadGeneration(const std::string &cacheKey, const std::string &generationId);

/// Persist a generation to disk: writes both `${cacheKey}/{id}.pcm` and
/// `${cacheKey}/{id}.json`. Creates the directory if missing. Writes via a
/// .tmp + rename so an interrupted save can never leave a half-written file
/// that loadGeneration would mistake for valid.
Result<void> saveGeneration(const std::string &cacheKey, const CachedGeneration &gen);

} // namespace creatures::voice
