#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

class TextToViseme;

/// One turn of a dialog scene as submitted by the controller.
struct DialogJobTurn {
    /// Stored creature id (UUID). Must exist in the creatures collection.
    std::string creatureId;
    /// Text to speak. May contain ElevenLabs inline audio tags like [giggles].
    std::string text;
};

/// Where the assembled animation gets stored on success.
enum class DialogPersistence {
    /// TTL collection — appears in ad-hoc listings, expires automatically.
    AdHoc,
    /// Normal animations collection — survives forever; show-prep target.
    Permanent,
};

/// Lifecycle of one job. Queued → Running → (Succeeded | Failed).
enum class DialogJobStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
};

/// Per-creature data the worker needs, resolved once at submit time so the
/// async worker doesn't re-look-up the DB and so submit-side validation
/// (distinct lanes, ≤10 voices, etc.) can fail synchronously.
struct DialogJobCreature {
    std::string creatureId;
    nlohmann::json creatureJson; // full stored doc; carries motors[], inputs[], mouth_slot
    std::string voiceId;
    uint16_t audioChannel; // 1-based
    uint8_t mouthSlot;
    universe_t universe; // resolved from creatureUniverseMap; used only on autoplay
};

/// One submitted job — owned by the manager, also held by the worker thread.
/// `status`/`animationId`/`errorMessage` are mutable via the worker; everything
/// else is immutable after submitJob returns.
struct DialogJob {
    std::string id;
    std::vector<DialogJobTurn> turns;
    std::vector<DialogJobCreature> creatures; // unique, indexed by creatureId
    DialogPersistence persistence;
    bool autoplay = false;
    std::string title;

    mutable std::mutex statusMutex;
    DialogJobStatus status = DialogJobStatus::Queued;
    std::string animationId;  // set on success
    std::string errorMessage; // set on failure
};

/// Polling snapshot the controller hands back to clients.
struct DialogJobStatusSnapshot {
    std::string id;
    DialogJobStatus status;
    std::string animationId;  // empty unless status == Succeeded
    std::string errorMessage; // empty unless status == Failed
};

/// Manager that owns the job registry and runs each worker on its own thread.
///
/// Singleton — there's one global manager per server process. Jobs live in the
/// registry indefinitely (the controller polls until it sees Succeeded/Failed
/// and is then done — the registry is intentionally retained so the client can
/// re-poll after picking up the final state).
class DialogJobManager {
  public:
    static DialogJobManager &instance();

    /// Submit a job. Performs all synchronous validation (creatures exist + are
    /// well-formed; distinct lanes; ≤10 unique voices; universe alignment if
    /// autoplay) and then kicks off the worker thread. Returns the new job id
    /// on success, or InvalidData if validation fails.
    ///
    /// All resolved per-creature info is cached on the job so the worker
    /// doesn't re-look-up the DB.
    Result<std::string> submitJob(std::vector<DialogJobTurn> turns, DialogPersistence persistence, bool autoplay,
                                  std::string title, std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /// Return a snapshot of the named job, or std::nullopt if unknown.
    std::optional<DialogJobStatusSnapshot> getStatus(const std::string &jobId);

  private:
    DialogJobManager() = default;

    /// The async worker — owns the full pipeline from chunkTurns through
    /// persistence + (optional) autoplay. Detached.
    void runJob(std::shared_ptr<DialogJob> job, std::shared_ptr<OperationSpan> jobSpan);

    /// Lazily load the CMU dictionary into a shared TextToViseme on first use.
    /// The dict is multi-MB; one copy per manager (not per job) saves the
    /// repeat load cost on a busy server.
    std::shared_ptr<TextToViseme> getTextToViseme();

    std::mutex registryMutex_;
    std::unordered_map<std::string, std::shared_ptr<DialogJob>> jobs_;

    std::mutex visemeMutex_;
    std::shared_ptr<TextToViseme> textToViseme_;
};

} // namespace creatures::voice
