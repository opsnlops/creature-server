#include "StreamingTTSClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#include <base64.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

// Internal SSL connection state
struct StreamingTTSClient::SSLConnection {
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    int sockfd = -1;

    ~SSLConnection() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx) {
            SSL_CTX_free(ctx);
        }
        if (sockfd >= 0) {
            close(sockfd);
        }
    }
};

StreamingTTSClient::StreamingTTSClient() = default;

StreamingTTSClient::~StreamingTTSClient() { disconnect(); }

void StreamingTTSClient::disconnect() { conn_.reset(); }

// WebSocket frame helpers
namespace {

// Generate a random 4-byte masking key
std::array<uint8_t, 4> generateMaskingKey() {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist;
    uint32_t key = dist(rng);
    std::array<uint8_t, 4> mask{};
    std::memcpy(mask.data(), &key, 4);
    return mask;
}

// Build a masked WebSocket frame (client must mask per RFC 6455)
std::vector<uint8_t> buildFrame(uint8_t opcode, const uint8_t *payload, size_t len) {
    std::vector<uint8_t> frame;
    frame.reserve(14 + len); // max header + payload

    // FIN bit + opcode
    frame.push_back(0x80 | opcode);

    // Mask bit always set for client frames
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(0x80 | len));
    } else if (len <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
        }
    }

    // 4-byte masking key
    auto mask = generateMaskingKey();
    frame.insert(frame.end(), mask.begin(), mask.end());

    // Masked payload
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }

    return frame;
}

std::vector<uint8_t> buildTextFrame(const std::string &text) {
    return buildFrame(0x01, reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

std::vector<uint8_t> buildCloseFrame() { return buildFrame(0x08, nullptr, 0); }

// Base64 encode (for WebSocket key)
static const char base64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t *data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) {
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        if (i + 2 < len) {
            n |= static_cast<uint32_t>(data[i + 2]);
        }

        result.push_back(base64Table[(n >> 18) & 0x3F]);
        result.push_back(base64Table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? base64Table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? base64Table[n & 0x3F] : '=');
    }

    return result;
}

// Read exactly n bytes from SSL
bool sslReadExact(SSL *ssl, uint8_t *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        int r = SSL_read(ssl, buf + total, static_cast<int>(n - total));
        if (r <= 0) {
            return false;
        }
        total += static_cast<size_t>(r);
    }
    return true;
}

// Send all bytes via SSL
bool sslWriteAll(SSL *ssl, const uint8_t *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        int w = SSL_write(ssl, data + total, static_cast<int>(len - total));
        if (w <= 0) {
            return false;
        }
        total += static_cast<size_t>(w);
    }
    return true;
}

} // namespace

Result<void> StreamingTTSClient::connectWebSocket(const std::string &host, uint16_t port, const std::string &path,
                                                   const std::string &apiKey,
                                                   std::shared_ptr<OperationSpan> parentSpan) {

    auto span = creatures::observability->createChildOperationSpan("StreamingTTSClient.connect", parentSpan);

    // DNS resolution
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    int err = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
    if (err != 0 || !result) {
        std::string msg = fmt::format("DNS resolution failed for {}: {}", host, gai_strerror(err));
        if (span) {
            span->setError(msg);
        }
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    conn_ = std::make_unique<SSLConnection>();

    // Create socket
    conn_->sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (conn_->sockfd < 0) {
        freeaddrinfo(result);
        std::string msg = fmt::format("Socket creation failed: {}", strerror(errno));
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    // Connect
    if (connect(conn_->sockfd, result->ai_addr, result->ai_addrlen) < 0) {
        freeaddrinfo(result);
        std::string msg = fmt::format("TCP connection to {}:{} failed: {}", host, port, strerror(errno));
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }
    freeaddrinfo(result);

    // TLS setup
    conn_->ctx = SSL_CTX_new(TLS_client_method());
    if (!conn_->ctx) {
        std::string msg = "SSL_CTX_new failed";
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    // Force HTTP/1.1 via ALPN — ElevenLabs defaults to h2 which doesn't
    // support the classic WebSocket upgrade handshake
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(conn_->ctx, alpn, sizeof(alpn));

    conn_->ssl = SSL_new(conn_->ctx);
    SSL_set_fd(conn_->ssl, conn_->sockfd);
    SSL_set_tlsext_host_name(conn_->ssl, host.c_str());

    if (SSL_connect(conn_->ssl) <= 0) {
        std::string msg = fmt::format("TLS handshake failed with {}", host);
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    debug("TLS connection established to {}:{}", host, port);

    // Generate WebSocket key
    uint8_t keyBytes[16];
    std::mt19937 rng(std::random_device{}());
    for (auto &b : keyBytes) {
        b = static_cast<uint8_t>(rng() & 0xFF);
    }
    std::string wsKey = base64Encode(keyBytes, 16);

    // Send HTTP upgrade request with xi-api-key header
    // ALPN must be set to http/1.1 above or the server negotiates h2 and rejects the upgrade
    std::string upgradeRequest = fmt::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "xi-api-key: {}\r\n"
        "\r\n",
        path, host, wsKey, apiKey);

    if (!sslWriteAll(conn_->ssl, reinterpret_cast<const uint8_t *>(upgradeRequest.data()), upgradeRequest.size())) {
        std::string msg = "Failed to send WebSocket upgrade request";
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    // Read HTTP response (read until \r\n\r\n)
    std::string response;
    char buf;
    int consecutiveNewlines = 0;
    while (true) {
        int r = SSL_read(conn_->ssl, &buf, 1);
        if (r <= 0) {
            break;
        }
        response.push_back(buf);

        if (buf == '\n') {
            if (consecutiveNewlines == 1) {
                break; // Found \r\n\r\n
            }
            consecutiveNewlines++;
        } else if (buf != '\r') {
            consecutiveNewlines = 0;
        }
    }

    // Verify 101 Switching Protocols
    if (response.find("101") == std::string::npos) {
        std::string msg = fmt::format("WebSocket upgrade failed. Response: {}", response.substr(0, 200));
        error(msg);
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    debug("WebSocket connection established to {}{}", host, path);
    if (span) {
        span->setSuccess();
    }

    return Result<void>{};
}

Result<void> StreamingTTSClient::sendTextFrame(const std::string &text) {
    if (!conn_ || !conn_->ssl) {
        return Result<void>{ServerError(ServerError::InternalError, "Not connected")};
    }

    auto frame = buildTextFrame(text);
    if (!sslWriteAll(conn_->ssl, frame.data(), frame.size())) {
        return Result<void>{ServerError(ServerError::InternalError, "Failed to send WebSocket text frame")};
    }

    return Result<void>{};
}

void StreamingTTSClient::sendCloseFrame() {
    if (!conn_ || !conn_->ssl) {
        return;
    }

    auto frame = buildCloseFrame();
    sslWriteAll(conn_->ssl, frame.data(), frame.size());
}

Result<StreamingTTSResult> StreamingTTSClient::receiveAllFrames(const std::string &outputFormat,
                                                                 ProgressCallback progressCallback,
                                                                 std::shared_ptr<OperationSpan> parentSpan) {
    if (!conn_ || !conn_->ssl) {
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, "Not connected")};
    }

    StreamingTTSResult result;
    result.audioFormat = outputFormat;

    bool running = true;
    size_t totalBytesReceived = 0;
    int wsFrameCount = 0;
    int audioChunkCount = 0;
    int alignmentChunkCount = 0;

    // Fragmentation state
    std::vector<uint8_t> fragmentBuffer;
    uint8_t fragmentOpcode = 0;
    int fragmentCount = 0;

    info("StreamingTTSClient: starting frame receive loop (format={})", outputFormat);

    while (running) {
        uint8_t header[2];
        if (!sslReadExact(conn_->ssl, header, 2)) {
            warn("StreamingTTSClient: connection closed reading frame header "
                 "(after {} frames, {} audio bytes)", wsFrameCount, result.audioData.size());
            break;
        }

        bool fin = (header[0] & 0x80) != 0;
        uint8_t opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t payloadLen = header[1] & 0x7F;

        // Extended payload length
        if (payloadLen == 126) {
            uint8_t ext[2];
            if (!sslReadExact(conn_->ssl, ext, 2)) {
                break;
            }
            payloadLen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (payloadLen == 127) {
            uint8_t ext[8];
            if (!sslReadExact(conn_->ssl, ext, 8)) {
                break;
            }
            payloadLen = 0;
            for (int i = 0; i < 8; ++i) {
                payloadLen = (payloadLen << 8) | ext[i];
            }
        }

        // Masking key (server shouldn't mask, but handle it)
        uint8_t mask[4] = {};
        if (masked) {
            if (!sslReadExact(conn_->ssl, mask, 4)) {
                break;
            }
        }

        // Read payload
        std::vector<uint8_t> payload(payloadLen);
        if (payloadLen > 0) {
            if (!sslReadExact(conn_->ssl, payload.data(), payloadLen)) {
                break;
            }
            if (masked) {
                for (size_t i = 0; i < payloadLen; ++i) {
                    payload[i] ^= mask[i % 4];
                }
            }
        }

        wsFrameCount++;
        totalBytesReceived += payloadLen;

        debug("StreamingTTSClient: ws frame #{}: opcode={} fin={} masked={} len={} totalBytes={}",
              wsFrameCount, opcode, fin, masked, payloadLen, totalBytesReceived);

        // Handle fragmentation
        if (opcode == 0x00) {
            fragmentCount++;
            fragmentBuffer.insert(fragmentBuffer.end(), payload.begin(), payload.end());
            debug("StreamingTTSClient: continuation frame #{}, buffer={} bytes", fragmentCount, fragmentBuffer.size());
            if (!fin) {
                continue;
            }
            opcode = fragmentOpcode;
            payload = std::move(fragmentBuffer);
            fragmentBuffer.clear();
            info("StreamingTTSClient: reassembled {} fragments -> {} bytes (opcode={})",
                 fragmentCount, payload.size(), opcode);
            fragmentOpcode = 0;
            fragmentCount = 0;
        } else if (!fin && (opcode == 0x01 || opcode == 0x02)) {
            fragmentOpcode = opcode;
            fragmentBuffer = std::move(payload);
            fragmentCount = 1;
            debug("StreamingTTSClient: fragment start (opcode={}), {} bytes", opcode, fragmentBuffer.size());
            continue;
        }

        switch (opcode) {
        case 0x01: {
            std::string text(payload.begin(), payload.end());

            // Log what fields exist in this JSON before parsing
            // Note: only check top-level JSON structure, not inside base64 payload
            // Use the parsed JSON below for reliable field detection
            bool textHasAudio = text.find("\"audio\"") != std::string::npos;
            bool textHasNull = text.find("\"audio\":null") != std::string::npos;
            bool textHasAlign = false; // detected from parsed JSON below
            bool textHasFinal = text.find("\"isFinal\":true") != std::string::npos;
            info("StreamingTTSClient: text frame #{}: {} bytes, audio={} audioNull={} alignment={} isFinal={} "
                 "first100='{}'",
                 wsFrameCount, text.size(), textHasAudio, textHasNull, textHasAlign, textHasFinal,
                 text.substr(0, 100));

            try {
                auto json = nlohmann::json::parse(text);

                if (json.contains("isFinal") && json["isFinal"].is_boolean() && json["isFinal"].get<bool>()) {
                    info("StreamingTTSClient: isFinal received after {} frames, {} audio chunks ({} bytes), "
                         "{} alignment chunks ({} chars)",
                         wsFrameCount, audioChunkCount, result.audioData.size(),
                         alignmentChunkCount, result.charTimings.size());
                    running = false;
                    break;
                }

                // Audio decoding
                if (json.contains("audio")) {
                    if (json["audio"].is_null()) {
                        debug("StreamingTTSClient: audio=null in frame #{}", wsFrameCount);
                    } else if (json["audio"].is_string()) {
                        auto audioB64 = json["audio"].get<std::string>();
                        if (audioB64.empty()) {
                            debug("StreamingTTSClient: audio=\"\" (empty) in frame #{}", wsFrameCount);
                        } else {
                            try {
                                auto decoded = base64::from_base64(audioB64);
                                result.audioData.insert(result.audioData.end(),
                                                        reinterpret_cast<const uint8_t *>(decoded.data()),
                                                        reinterpret_cast<const uint8_t *>(decoded.data()) +
                                                            decoded.size());
                                audioChunkCount++;
                                info("StreamingTTSClient: audio chunk {}: {} b64 chars -> {} bytes decoded "
                                     "(cumulative: {} bytes)",
                                     audioChunkCount, audioB64.size(), decoded.size(), result.audioData.size());
                            } catch (const std::exception &e) {
                                error("StreamingTTSClient: BASE64 DECODE FAILED: {} "
                                      "({} chars, first80='{}')",
                                      e.what(), audioB64.size(), audioB64.substr(0, 80));
                            }
                        }
                    } else {
                        warn("StreamingTTSClient: audio is unexpected JSON type: {}", json["audio"].type_name());
                    }
                } else {
                    debug("StreamingTTSClient: no 'audio' key in frame #{}", wsFrameCount);
                }

                // Alignment (times are absolute, not per-chunk)
                if (json.contains("alignment") && !json["alignment"].is_null()) {
                    auto &al = json["alignment"];
                    if (al.contains("chars") && al.contains("charStartTimesMs") && al.contains("charDurationsMs")) {
                        auto chars = al["chars"].get<std::vector<std::string>>();
                        auto starts = al["charStartTimesMs"].get<std::vector<double>>();
                        auto durs = al["charDurationsMs"].get<std::vector<double>>();
                        size_t count = std::min({chars.size(), starts.size(), durs.size()});
                        std::string chunkText;

                        for (size_t i = 0; i < count; ++i) {
                            TextToViseme::CharTiming ct;
                            ct.character = chars[i].empty() ? ' ' : chars[i][0];
                            ct.startTimeMs = starts[i];
                            ct.durationMs = durs[i];
                            result.charTimings.push_back(ct);
                            result.alignmentText.push_back(ct.character);
                            chunkText.push_back(ct.character);
                        }
                        alignmentChunkCount++;
                        info("StreamingTTSClient: alignment chunk {}: {} chars, text='{}' (cumulative: {})",
                             alignmentChunkCount, count, chunkText, result.charTimings.size());
                    }
                }
            } catch (const std::exception &e) {
                error("StreamingTTSClient: EXCEPTION in frame #{}: {} size={} preview='{}'",
                      wsFrameCount, e.what(), text.size(), text.substr(0, 200));
            }
            break;
        }

        case 0x02:
            result.audioData.insert(result.audioData.end(), payload.begin(), payload.end());
            info("StreamingTTSClient: binary frame: {} bytes (cumulative: {})", payloadLen, result.audioData.size());
            break;

        case 0x08:
            info("StreamingTTSClient: close frame after {} frames", wsFrameCount);
            running = false;
            break;

        case 0x09: {
            auto pong = buildFrame(0x0A, payload.data(), payload.size());
            sslWriteAll(conn_->ssl, pong.data(), pong.size());
            debug("StreamingTTSClient: ping -> pong");
            break;
        }

        case 0x0A:
            break;

        default:
            warn("StreamingTTSClient: unknown opcode {} in frame #{}", opcode, wsFrameCount);
            break;
        }

        if (progressCallback && totalBytesReceived > 0) {
            float progress = std::min(0.9f, static_cast<float>(totalBytesReceived) / 500000.0f);
            progressCallback(progress);
        }
    }

    // Estimate audio duration
    if (outputFormat.find("pcm") != std::string::npos) {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 88200.0;
    } else if (outputFormat.find("mp3") != std::string::npos) {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 24000.0;
    }

    info("StreamingTTSClient COMPLETE: {} ws frames, {} audio chunks ({} bytes), "
         "{} alignment chunks ({} chars), est {:.2f}s",
         wsFrameCount, audioChunkCount, result.audioData.size(),
         alignmentChunkCount, result.charTimings.size(), result.audioDurationSeconds);

    if (result.audioData.empty()) {
        error("StreamingTTSClient: ZERO audio bytes received! Review frame log above.");
    }

    return result;
}

Result<StreamingTTSResult> StreamingTTSClient::generateSpeech(const std::string &apiKey, const std::string &voiceId,
                                                               const std::string &modelId, const std::string &text,
                                                               const std::string &outputFormat, float stability,
                                                               float similarityBoost,
                                                               ProgressCallback progressCallback,
                                                               std::shared_ptr<OperationSpan> parentSpan) {

    auto span = creatures::observability->createChildOperationSpan("StreamingTTSClient.generateSpeech", parentSpan);
    if (span) {
        span->setAttribute("voice.id", voiceId);
        span->setAttribute("voice.model", modelId);
        span->setAttribute("text.length", static_cast<int64_t>(text.size()));
        span->setAttribute("audio.format", outputFormat);
    }

    if (progressCallback) {
        progressCallback(0.05f);
    }

    // Build WebSocket URL path with sync_alignment for character-level timing data
    std::string path = fmt::format(
        "/v1/text-to-speech/{}/stream-input?model_id={}&output_format={}&sync_alignment=true", voiceId, modelId,
        outputFormat);

    // Connect
    auto connectResult = connectWebSocket("api.elevenlabs.io", 443, path, apiKey, span);
    if (!connectResult.isSuccess()) {
        if (span) {
            span->setError(connectResult.getError()->getMessage());
        }
        return Result<StreamingTTSResult>{connectResult.getError().value()};
    }

    if (progressCallback) {
        progressCallback(0.10f);
    }

    // Send BOS (Begin of Stream) message with generation config
    // The API key and alignment request go in this first message
    nlohmann::json bosMessage;
    bosMessage["text"] = " "; // BOS requires a space
    bosMessage["voice_settings"]["stability"] = stability;
    bosMessage["voice_settings"]["similarity_boost"] = similarityBoost;
    bosMessage["xi_api_key"] = apiKey;
    bosMessage["try_trigger_generation"] = false;

    auto sendBosResult = sendTextFrame(bosMessage.dump());
    if (!sendBosResult.isSuccess()) {
        disconnect();
        if (span) {
            span->setError(sendBosResult.getError()->getMessage());
        }
        return Result<StreamingTTSResult>{sendBosResult.getError().value()};
    }

    // Send the actual text
    nlohmann::json textMessage;
    textMessage["text"] = text;
    textMessage["try_trigger_generation"] = true;

    auto sendTextResult = sendTextFrame(textMessage.dump());
    if (!sendTextResult.isSuccess()) {
        disconnect();
        if (span) {
            span->setError(sendTextResult.getError()->getMessage());
        }
        return Result<StreamingTTSResult>{sendTextResult.getError().value()};
    }

    // Send EOS (End of Stream)
    nlohmann::json eosMessage;
    eosMessage["text"] = "";

    auto sendEosResult = sendTextFrame(eosMessage.dump());
    if (!sendEosResult.isSuccess()) {
        disconnect();
        if (span) {
            span->setError(sendEosResult.getError()->getMessage());
        }
        return Result<StreamingTTSResult>{sendEosResult.getError().value()};
    }

    if (progressCallback) {
        progressCallback(0.15f);
    }

    // Receive all frames
    auto receiveResult = receiveAllFrames(outputFormat, progressCallback, span);
    disconnect();

    if (!receiveResult.isSuccess()) {
        if (span) {
            span->setError(receiveResult.getError()->getMessage());
        }
        return receiveResult;
    }

    auto ttsResult = receiveResult.getValue().value();

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(ttsResult.audioData.size()));
        span->setAttribute("alignment.chars", static_cast<int64_t>(ttsResult.charTimings.size()));
        span->setAttribute("audio.duration_s", ttsResult.audioDurationSeconds);
        span->setSuccess();
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    return ttsResult;
}

Result<StreamingTTSResult> StreamingTTSClient::generateSpeechREST(
    const std::string &apiKey, const std::string &voiceId, const std::string &modelId, const std::string &text,
    const std::string &outputFormat, float stability, float similarityBoost,
    const std::vector<std::string> &previousRequestIds, ProgressCallback progressCallback,
    std::shared_ptr<OperationSpan> parentSpan) {

    auto span =
        creatures::observability->createChildOperationSpan("StreamingTTSClient.generateSpeechREST", parentSpan);
    if (span) {
        span->setAttribute("voice.id", voiceId);
        span->setAttribute("voice.model", modelId);
        span->setAttribute("text.length", static_cast<int64_t>(text.size()));
        span->setAttribute("audio.format", outputFormat);
        span->setAttribute("previous_request_ids.count", static_cast<int64_t>(previousRequestIds.size()));
    }

    if (progressCallback) {
        progressCallback(0.05f);
    }

    // Build JSON request body
    nlohmann::json body;
    body["text"] = text;
    body["model_id"] = modelId;
    body["voice_settings"]["stability"] = stability;
    body["voice_settings"]["similarity_boost"] = similarityBoost;
    if (!previousRequestIds.empty()) {
        body["previous_request_ids"] = previousRequestIds;
    }

    std::string bodyStr = body.dump();

    // Build URL path
    std::string path = fmt::format("/v1/text-to-speech/{}/stream/with-timestamps?output_format={}", voiceId,
                                    outputFormat);

    // Connect via TLS
    auto connectSpan =
        creatures::observability->createChildOperationSpan("StreamingTTSClient.REST.connect", span);

    conn_ = std::make_unique<SSLConnection>();
    const std::string host = "api.elevenlabs.io";

    // DNS
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || !res) {
        std::string msg = "DNS resolution failed for " + host;
        if (span) span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    conn_->sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(conn_->sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        conn_.reset();
        std::string msg = "TCP connection failed to " + host;
        if (span) span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }
    freeaddrinfo(res);

    conn_->ctx = SSL_CTX_new(TLS_client_method());
    // Force HTTP/1.1 via ALPN
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(conn_->ctx, alpn, sizeof(alpn));

    conn_->ssl = SSL_new(conn_->ctx);
    SSL_set_fd(conn_->ssl, conn_->sockfd);
    SSL_set_tlsext_host_name(conn_->ssl, host.c_str());

    if (SSL_connect(conn_->ssl) <= 0) {
        conn_.reset();
        std::string msg = "TLS handshake failed with " + host;
        if (span) span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    if (connectSpan) connectSpan->setSuccess();

    if (progressCallback) {
        progressCallback(0.10f);
    }

    // Send HTTP POST request
    std::string httpRequest = fmt::format(
        "POST {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: {}\r\n"
        "xi-api-key: {}\r\n"
        "Accept: text/event-stream\r\n"
        "\r\n"
        "{}",
        path, host, bodyStr.size(), apiKey, bodyStr);

    if (!sslWriteAll(conn_->ssl, reinterpret_cast<const uint8_t *>(httpRequest.data()), httpRequest.size())) {
        conn_.reset();
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, "Failed to send HTTP request")};
    }

    // Read HTTP response headers
    std::string responseHeaders;
    char c;
    int newlines = 0;
    while (true) {
        if (SSL_read(conn_->ssl, &c, 1) <= 0) break;
        responseHeaders.push_back(c);
        if (c == '\n') {
            if (newlines == 1) break;
            newlines++;
        } else if (c != '\r') {
            newlines = 0;
        }
    }

    // Check for 200 OK
    if (responseHeaders.find("200") == std::string::npos) {
        conn_.reset();
        std::string msg = fmt::format("ElevenLabs REST TTS failed: {}", responseHeaders.substr(0, 200));
        error(msg);
        if (span) span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    // Extract request-id from response headers
    StreamingTTSResult result;
    result.audioFormat = outputFormat;

    auto reqIdPos = responseHeaders.find("request-id: ");
    if (reqIdPos == std::string::npos) {
        reqIdPos = responseHeaders.find("Request-Id: ");
    }
    if (reqIdPos == std::string::npos) {
        reqIdPos = responseHeaders.find("x-request-id: ");
    }
    if (reqIdPos != std::string::npos) {
        auto start = responseHeaders.find(": ", reqIdPos) + 2;
        auto end = responseHeaders.find("\r", start);
        if (end == std::string::npos) end = responseHeaders.find("\n", start);
        if (end != std::string::npos) {
            result.requestId = responseHeaders.substr(start, end - start);
            debug("StreamingTTSClient REST: request-id={}", result.requestId);
        }
    }

    if (progressCallback) {
        progressCallback(0.15f);
    }

    // Check if response is chunked transfer encoding
    bool isChunked = responseHeaders.find("Transfer-Encoding: chunked") != std::string::npos ||
                     responseHeaders.find("transfer-encoding: chunked") != std::string::npos;

    // Read streaming JSON response — newline-delimited JSON objects
    // Each contains audio_base64 and alignment data
    int chunkCount = 0;
    std::string lineBuffer;

    auto readLine = [&]() -> std::string {
        std::string line;
        char ch;
        while (SSL_read(conn_->ssl, &ch, 1) > 0) {
            if (ch == '\n') {
                return line;
            }
            if (ch != '\r') {
                line.push_back(ch);
            }
        }
        return line;
    };

    while (true) {
        std::string line;

        if (isChunked) {
            // Read chunk size
            std::string chunkSizeLine = readLine();
            if (chunkSizeLine.empty()) continue;

            size_t chunkSize = 0;
            try {
                chunkSize = std::stoull(chunkSizeLine, nullptr, 16);
            } catch (...) {
                continue;
            }

            if (chunkSize == 0) {
                break; // End of chunked response
            }

            // Read chunk data
            std::vector<uint8_t> chunkData(chunkSize);
            if (!sslReadExact(conn_->ssl, chunkData.data(), chunkSize)) {
                break;
            }
            line = std::string(chunkData.begin(), chunkData.end());

            // Read trailing \r\n after chunk
            char cr, lf;
            SSL_read(conn_->ssl, &cr, 1);
            SSL_read(conn_->ssl, &lf, 1);
        } else {
            line = readLine();
        }

        if (line.empty()) continue;

        // Parse JSON
        try {
            auto json = nlohmann::json::parse(line);

            // Decode audio
            if (json.contains("audio_base64") && json["audio_base64"].is_string()) {
                auto audioB64 = json["audio_base64"].get<std::string>();
                if (!audioB64.empty()) {
                    try {
                        auto decoded = base64::from_base64(audioB64);
                        result.audioData.insert(result.audioData.end(),
                                                reinterpret_cast<const uint8_t *>(decoded.data()),
                                                reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
                        chunkCount++;
                    } catch (const std::exception &e) {
                        warn("StreamingTTSClient REST: base64 decode failed: {}", e.what());
                    }
                }
            }

            // Parse alignment (REST uses seconds, not milliseconds)
            if (json.contains("alignment") && !json["alignment"].is_null()) {
                auto &al = json["alignment"];
                if (al.contains("characters") && al.contains("character_start_times_seconds") &&
                    al.contains("character_end_times_seconds")) {
                    auto chars = al["characters"].get<std::vector<std::string>>();
                    auto starts = al["character_start_times_seconds"].get<std::vector<double>>();
                    auto ends = al["character_end_times_seconds"].get<std::vector<double>>();

                    size_t count = std::min({chars.size(), starts.size(), ends.size()});
                    for (size_t i = 0; i < count; ++i) {
                        TextToViseme::CharTiming ct;
                        ct.character = chars[i].empty() ? ' ' : chars[i][0];
                        ct.startTimeMs = starts[i] * 1000.0; // seconds → milliseconds
                        ct.durationMs = (ends[i] - starts[i]) * 1000.0;
                        result.charTimings.push_back(ct);
                        result.alignmentText.push_back(ct.character);
                    }
                }
            }
        } catch (const nlohmann::json::exception &) {
            // Not JSON — might be end of stream marker or empty line
        }

        if (progressCallback && !result.audioData.empty()) {
            float progress = std::min(0.9f, static_cast<float>(result.audioData.size()) / 500000.0f);
            progressCallback(progress);
        }
    }

    disconnect();

    // Estimate duration
    if (outputFormat.find("mp3") != std::string::npos) {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 24000.0;
    } else if (outputFormat.find("pcm") != std::string::npos) {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 88200.0;
    }

    info("StreamingTTSClient REST complete: {} chunks, {} bytes audio, {} alignment chars, "
         "request_id={}, {:.2f}s estimated",
         chunkCount, result.audioData.size(), result.charTimings.size(), result.requestId,
         result.audioDurationSeconds);

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(result.audioData.size()));
        span->setAttribute("alignment.chars", static_cast<int64_t>(result.charTimings.size()));
        span->setAttribute("audio.duration_s", result.audioDurationSeconds);
        span->setAttribute("request_id", result.requestId);
        span->setAttribute("chunks", static_cast<int64_t>(chunkCount));
        span->setSuccess();
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    return result;
}

} // namespace creatures::voice
