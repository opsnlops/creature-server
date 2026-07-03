
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * Generate one chunk's dialog audio + forced alignment and cache it.
 *
 * The one shared implementation of the generate → align → save-to-cache block used by
 * the preview controller (single and chunked paths) and the render job's chunk loop.
 * Builds the 80-char turns summary, mints the generation id, and saves under `cacheKey`.
 * A cache-save failure is logged, not fatal — the caller still gets the generation.
 */
Result<CachedGeneration> generateChunkWithAlignment(DialogClient &client, const std::string &apiKey,
                                                    const std::vector<DialogInput> &chunk, const std::string &cacheKey,
                                                    std::shared_ptr<OperationSpan> span = nullptr);

/**
 * Merge per-chunk generations into ONE whole-scene generation for auditioning.
 *
 * Long scenes are generated as multiple ElevenLabs calls (chunkTurns); the preview
 * panel wants a single playable take. The merge:
 *  - concatenates chunk PCM with INTER_CHUNK_GAP_SECS of silence at each seam (the
 *    same pause the render inserts, so the audition sounds like the show will)
 *  - offsets forced-alignment word/char times by each chunk's start (including gaps),
 *    inserting a synthetic space character across each seam so the character stream
 *    matches the whole-scene transcript convention (turns joined with single spaces)
 *  - offsets voice-segment character indices by prior chunks' character-array lengths
 *    (+1 per joining space) and dialogInputIndex by turns-before-chunk
 *  - reports the WORST chunk's forced-alignment loss (conservative)
 *
 * `turnCounts[i]` is the number of turns in chunk i. The caller sets `turnsSummary`
 * (the merged take should summarize the whole scene, which the chunks can't see).
 */
CachedGeneration mergeChunkGenerations(const std::vector<CachedGeneration> &chunkGens,
                                       const std::vector<std::size_t> &turnCounts, uint32_t sampleRate = 48000,
                                       double interChunkGapSecs = dialog_pipeline::INTER_CHUNK_GAP_SECS);

} // namespace creatures::voice
