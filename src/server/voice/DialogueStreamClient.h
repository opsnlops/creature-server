#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "StreamingTTSClient.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace ix {
class WebSocket;
}

namespace creatures::voice {

/**
 * A persistent connection to ElevenLabs' text-to-dialogue WebSocket, which is
 * the only place the v4 family is served (issue #248).
 *
 * Why this exists alongside StreamingTTSClient rather than inside it: the two
 * speak different endpoints over different transports. The text-to-speech
 * client hand-rolls TLS and framing over OpenSSL and carries fixes earned in
 * production (#189, #190), so it is left alone. This one uses IXWebSocket —
 * the same client the controller already uses — so there is no second copy of
 * WebSocket framing anywhere. What the two genuinely share, the alignment
 * parsing, is shared: see appendAlignmentChunk and wireKeys.
 *
 * ## Why a connection rather than a call
 *
 * A dialogue connection outlives many turns on purpose. Every turn after the
 * first renders with the earlier ones in context, which is where the
 * performance comes from, and keeping the socket warm is also what holds time
 * to first audio at ~140 ms — faster than the text-to-speech socket manages
 * with the fast models.
 *
 * ## The two things that are easy to get wrong
 *
 * `voices` must be the first message and must name every voice the session
 * will ever use; the socket refuses a voice_id that was not registered there.
 * And every turn must be followed by an explicit flush, or its audio boundary
 * is withheld until the *next* turn arrives — so a turn never completes on
 * time, and a lone turn produces no audio at all.
 *
 * ## Threading
 *
 * IXWebSocket delivers frames on its own thread; renderTurn() blocks the
 * caller until that thread reports the turn finished. One turn at a time per
 * client, which is what the render pipeline does anyway.
 */
class DialogueStreamClient {
  public:
    DialogueStreamClient();
    ~DialogueStreamClient();

    DialogueStreamClient(const DialogueStreamClient &) = delete;
    DialogueStreamClient &operator=(const DialogueStreamClient &) = delete;

    /**
     * Open the socket and register every voice the session may use.
     *
     * @param voiceIds every participant's voice. A turn naming anything else
     *        is refused by the server, so this has to be the whole cast.
     * @param outputFormat an ElevenLabs format string, e.g. "pcm_48000".
     */
    Result<void> connect(const std::string &apiKey, const std::vector<std::string> &voiceIds,
                         const std::string &modelId, const std::string &outputFormat,
                         std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Render one turn, blocking until the server marks it final.
     *
     * Re-opens the socket first if it has gone away — it closes itself after
     * 20 s of silence, which an agent-driven exchange reaches whenever the
     * model pauses to think. A reconnect costs the shared context rather than
     * the turn, so it is recorded on the span as `dialogue.reconnected` and
     * the turn proceeds.
     */
    Result<StreamingTTSResult> renderTurn(const std::string &voiceId, const std::string &text,
                                          std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /// True while the socket is believed open and its voices registered.
    [[nodiscard]] bool connected() const;

    /// Close the socket. Safe to call more than once.
    void close();

  private:
    /// (Re-)open the socket and send the voice registration.
    Result<void> openSocket(const std::shared_ptr<OperationSpan> &parentSpan);

    /// Called on IXWebSocket's thread for each text frame.
    void onTextFrame(const std::string &payload);

    std::unique_ptr<ix::WebSocket> ws_;

    // Kept so a turn can re-open the socket without the caller caring.
    std::string apiKey_;
    std::vector<std::string> voiceIds_;
    std::string modelId_;
    std::string outputFormat_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool open_{false};
    bool turnComplete_{false};
    std::string failure_;     ///< non-empty once the socket has failed the turn
    StreamingTTSResult turn_; ///< accumulates while a turn is in flight
    /// Where the next alignment chunk begins. This socket restarts every
    /// chunk's times at zero, so without this the whole turn stacks on top of
    /// itself and the beak barely moves (issue #252). Reset per turn.
    double alignmentOffsetMs_{0.0};
};

} // namespace creatures::voice
