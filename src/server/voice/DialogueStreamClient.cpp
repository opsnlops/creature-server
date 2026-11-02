#include "DialogueStreamClient.h"

#include <chrono>
#include <exception>
#include <utility>

#include <base64.hpp>
#include <fmt/format.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

namespace {

/// How long to wait for a connection to come up. Generous: a cold TLS
/// handshake across the public internet is not a hot path.
constexpr auto kConnectTimeout = std::chrono::seconds(15);

/// How long one turn may take. Generation runs several times faster than
/// real time, so this is an upper bound on pathology rather than a budget.
constexpr auto kTurnTimeout = std::chrono::seconds(60);

/// Idle close the server applies. Nothing enforces it here; it is why
/// renderTurn() has to tolerate finding the socket gone.
constexpr int kPingIntervalSeconds = 10;

} // namespace

DialogueStreamClient::DialogueStreamClient() = default;

DialogueStreamClient::~DialogueStreamClient() { close(); }

bool DialogueStreamClient::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_ && ws_ != nullptr;
}

void DialogueStreamClient::close() {
    std::unique_ptr<ix::WebSocket> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
        doomed = std::move(ws_);
    }
    // Dropped outside the lock: stop() joins IXWebSocket's thread, and that
    // thread takes the same mutex in onTextFrame().
    if (doomed) {
        doomed->setOnMessageCallback(nullptr);
        doomed->stop();
    }
}

Result<void> DialogueStreamClient::connect(const std::string &apiKey, const std::vector<std::string> &voiceIds,
                                           const std::string &modelId, const std::string &outputFormat,
                                           std::shared_ptr<OperationSpan> parentSpan) {
    if (voiceIds.empty()) {
        return Result<void>{
            ServerError(ServerError::InvalidData, "a dialogue connection needs at least one voice to register")};
    }
    if (apiKey.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "no ElevenLabs API key configured")};
    }

    apiKey_ = apiKey;
    voiceIds_ = voiceIds;
    modelId_ = modelId;
    outputFormat_ = outputFormat;

    return openSocket(parentSpan);
}

Result<void> DialogueStreamClient::openSocket(const std::shared_ptr<OperationSpan> &parentSpan) {
    close();

    auto socket = std::make_unique<ix::WebSocket>();
    const std::string url = fmt::format("wss://api.elevenlabs.io/v1/text-to-dialogue/stream-input"
                                        "?model_id={}&output_format={}&sync_alignment=true",
                                        modelId_, outputFormat_);
    socket->setUrl(url);
    socket->setExtraHeaders({{"xi-api-key", apiKey_}});
    // We decide when to re-open, in renderTurn(). Letting the library do it
    // silently would hand us a fresh socket with no voices registered, whose
    // every turn would then be refused.
    socket->disableAutomaticReconnection();
    socket->setPingInterval(kPingIntervalSeconds);

    std::condition_variable *cv = &cv_;
    socket->setOnMessageCallback([this, cv](const ix::WebSocketMessagePtr &msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Message:
            onTextFrame(msg->str);
            break;
        case ix::WebSocketMessageType::Open: {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
            cv->notify_all();
            break;
        }
        case ix::WebSocketMessageType::Close: {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
            if (failure_.empty()) {
                failure_ = "dialogue socket closed";
            }
            cv->notify_all();
            break;
        }
        case ix::WebSocketMessageType::Error: {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
            failure_ = fmt::format("dialogue socket error: {}", msg->errorInfo.reason);
            cv->notify_all();
            break;
        }
        default:
            break;
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_.clear();
        ws_ = std::move(socket);
        ws_->start();
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, kConnectTimeout, [this] { return open_ || !failure_.empty(); })) {
        const std::string msg = "timed out opening the dialogue socket";
        lock.unlock();
        close();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }
    if (!open_) {
        const std::string msg = failure_.empty() ? "dialogue socket did not open" : failure_;
        lock.unlock();
        close();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    // Must be first, and must be bare strings — objects are rejected. Any
    // voice missing here is refused for the life of the connection.
    nlohmann::json registration;
    registration["voices"] = voiceIds_;
    ws_->sendUtf8Text(registration.dump());
    lock.unlock();

    if (parentSpan) {
        parentSpan->setAttribute("dialogue.model", modelId_);
        parentSpan->setAttribute("dialogue.voices", static_cast<int64_t>(voiceIds_.size()));
    }
    info("dialogue socket open on {} with {} voice(s)", modelId_, voiceIds_.size());
    return Result<void>{};
}

void DialogueStreamClient::onTextFrame(const std::string &payload) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(payload);
    } catch (const std::exception &e) {
        warn("dialogue socket sent unparseable JSON ({} bytes): {}", payload.size(), e.what());
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // The server reports its own problems in-band, including the idle close.
    if (json.contains("error")) {
        const auto code = json.value("error", std::string{"unknown"});
        failure_ =
            fmt::format("dialogue socket reported '{}': {}", code, json.value("message", std::string{"no message"}));
        open_ = false;
        cv_.notify_all();
        return;
    }

    if (json.contains("audio") && json["audio"].is_string()) {
        const auto encoded = json["audio"].get<std::string>();
        if (!encoded.empty()) {
            try {
                const auto decoded = base64::from_base64(encoded);
                turn_.audioData.insert(turn_.audioData.end(), reinterpret_cast<const uint8_t *>(decoded.data()),
                                       reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
            } catch (const std::exception &e) {
                warn("dialogue socket audio failed base64 decode ({} chars): {}", encoded.size(), e.what());
            }
        }
    }

    if (json.contains("alignment") && !json["alignment"].is_null()) {
        appendAlignmentChunk(json["alignment"], WireDialect::Dialogue, turn_.charTimings, turn_.alignmentText);
    }

    // Checked last on purpose: this frame usually carries the turn's final
    // audio and alignment, and dropping it would clip the tail of the mouth
    // movement.
    const char *finalKey = wireKeys(WireDialect::Dialogue).final;
    if (json.contains(finalKey) && json[finalKey].is_boolean() && json[finalKey].get<bool>()) {
        turnComplete_ = true;
        cv_.notify_all();
    }
}

Result<StreamingTTSResult> DialogueStreamClient::renderTurn(const std::string &voiceId, const std::string &text,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto span =
        creatures::observability->createChildOperationSpan("DialogueStreamClient.renderTurn", std::move(parentSpan));
    if (span) {
        span->setAttribute("voice.id", voiceId);
        span->setAttribute("voice.model", modelId_);
        span->setAttribute("text.length", static_cast<int64_t>(text.size()));
        span->setAttribute("audio.format", outputFormat_);
    }

    if (voiceIds_.empty()) {
        const std::string msg = "renderTurn called before connect";
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    const bool reconnected = !connected();
    if (reconnected) {
        auto again = openSocket(span);
        if (!again.isSuccess()) {
            if (span)
                span->setError(again.getError()->getMessage());
            return Result<StreamingTTSResult>{again.getError().value()};
        }
    }
    if (span) {
        span->setAttribute("dialogue.reconnected", reconnected);
    }

    nlohmann::json turn;
    turn["inputs"] = nlohmann::json::array({{{"text", text}, {"voice_id", voiceId}}});
    nlohmann::json flush;
    flush["flush"] = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        turn_ = StreamingTTSResult{};
        turn_.audioFormat = outputFormat_;
        turnComplete_ = false;
        failure_.clear();
        ws_->sendUtf8Text(turn.dump());
        // Not optional: without it this turn's boundary waits for the NEXT
        // turn, so it never completes and a lone turn yields no audio at all.
        ws_->sendUtf8Text(flush.dump());
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool settled = cv_.wait_for(lock, kTurnTimeout, [this] { return turnComplete_ || !failure_.empty(); });

    if (!settled) {
        lock.unlock();
        close();
        const std::string msg = fmt::format("dialogue turn for voice {} timed out", voiceId);
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }
    if (!turnComplete_) {
        const std::string msg = failure_;
        lock.unlock();
        close();
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    auto result = turn_;
    lock.unlock();

    result.audioDurationSeconds = estimateAudioSeconds(outputFormat_, result.audioData.size());

    if (result.audioData.empty()) {
        // Almost always an unregistered voice or a missing flush; both arrive
        // as a well-formed stream carrying nothing.
        const std::string msg = fmt::format("dialogue turn for voice {} returned no audio", voiceId);
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(result.audioData.size()));
        span->setAttribute("alignment.chars", static_cast<int64_t>(result.charTimings.size()));
        span->setAttribute("audio.duration_s", result.audioDurationSeconds);
        span->setSuccess();
    }
    return result;
}

} // namespace creatures::voice
