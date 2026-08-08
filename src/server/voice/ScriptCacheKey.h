#pragma once

#include <memory>
#include <string>
#include <vector>

#include "model/DialogScript.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/// Compute the whole-scene dialog cache key for a script's current turns
/// (#131).
///
/// This is the value an accepted take's `dialog_cache_key` is compared
/// against: equal means the acceptance still describes the script as it
/// stands, different means the turns have moved on and the take is stale.
///
/// Whole-scene deliberately. The preview service keys its assembled take the
/// same way — `computeCacheKey(all inputs)` — while chunk keys exist only as
/// an internal cache inside that assembly. Hashing per chunk here would never
/// match what was accepted.
///
/// Resolving a creature to its voice id needs the stored creature JSON, since
/// `Creature` doesn't carry the voice block. Returns InvalidData when a turn
/// names a creature that has no voice configured — that's the same condition
/// the render would fail on later, surfaced earlier and more clearly.
[[nodiscard]] Result<std::string> computeScriptCacheKey(const std::vector<creatures::DialogScriptTurn> &turns,
                                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);

/// Whether an accepted take still matches the script's current turns.
/// `nullopt` means the key could not be computed at all (a creature is
/// missing or has no voice) — distinct from "computed, and it doesn't match".
[[nodiscard]] std::optional<bool> acceptedVoiceIsFresh(const creatures::DialogScript &script,
                                                       std::shared_ptr<OperationSpan> parentSpan = nullptr);

} // namespace creatures::voice
