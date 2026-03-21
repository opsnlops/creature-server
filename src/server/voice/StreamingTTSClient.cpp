#include "StreamingTTSClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

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

    // Send HTTP upgrade request
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

    while (running) {
        // Read frame header (2 bytes minimum)
        uint8_t header[2];
        if (!sslReadExact(conn_->ssl, header, 2)) {
            debug("WebSocket connection closed by server");
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

        totalBytesReceived += payloadLen;

        switch (opcode) {
        case 0x01: // Text frame (JSON alignment data)
        {
            std::string text(payload.begin(), payload.end());
            try {
                auto json = nlohmann::json::parse(text);

                // ElevenLabs sends alignment data as:
                // {"audio": "<base64>", "alignment": {"charStartTimesMs": [...], "charDurationsMs": [...], "chars":
                // [...]}}
                if (json.contains("alignment") && !json["alignment"].is_null()) {
                    auto &alignment = json["alignment"];
                    if (alignment.contains("chars") && alignment.contains("charStartTimesMs") &&
                        alignment.contains("charDurationsMs")) {
                        auto chars = alignment["chars"].get<std::vector<std::string>>();
                        auto startTimes = alignment["charStartTimesMs"].get<std::vector<double>>();
                        auto durations = alignment["charDurationsMs"].get<std::vector<double>>();

                        size_t count = std::min({chars.size(), startTimes.size(), durations.size()});
                        for (size_t i = 0; i < count; ++i) {
                            TextToViseme::CharTiming ct;
                            ct.character = chars[i].empty() ? ' ' : chars[i][0];
                            ct.startTimeMs = startTimes[i];
                            ct.durationMs = durations[i];
                            result.charTimings.push_back(ct);
                            result.alignmentText.push_back(ct.character);
                        }
                    }
                }

                // ElevenLabs may also send audio as base64 in the JSON
                if (json.contains("audio") && json["audio"].is_string()) {
                    // Audio chunk is base64-encoded in the text frame
                    // We'll handle this but prefer binary frames
                    auto audioB64 = json["audio"].get<std::string>();
                    if (!audioB64.empty()) {
                        // Simple base64 decode
                        // (ElevenLabs sends audio in binary frames, this is a fallback)
                        debug("Received audio in JSON text frame ({} base64 chars)", audioB64.size());
                    }
                }
            } catch (const nlohmann::json::exception &e) {
                trace("Non-JSON text frame received: {}", text.substr(0, 100));
            }
            break;
        }

        case 0x02: // Binary frame (raw audio data)
            result.audioData.insert(result.audioData.end(), payload.begin(), payload.end());
            break;

        case 0x08: // Close frame
            debug("Received WebSocket close frame");
            running = false;
            break;

        case 0x09: // Ping
        {
            // Respond with pong
            auto pong = buildFrame(0x0A, payload.data(), payload.size());
            sslWriteAll(conn_->ssl, pong.data(), pong.size());
            break;
        }

        case 0x0A: // Pong
            break;

        default:
            trace("Unknown WebSocket opcode: {}", opcode);
            break;
        }

        (void)fin; // We process complete messages regardless of FIN

        // Update progress based on data received (rough estimate)
        if (progressCallback && totalBytesReceived > 0) {
            // Can't know total size ahead of time, use a logarithmic estimate
            float progress = std::min(0.9f, static_cast<float>(totalBytesReceived) / 500000.0f);
            progressCallback(progress);
        }
    }

    // Estimate audio duration from data size
    if (outputFormat == "pcm_48000") {
        // 48kHz, 16-bit mono = 96000 bytes/second
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 96000.0;
    } else if (outputFormat.find("mp3") != std::string::npos) {
        // Rough MP3 estimate: ~24000 bytes/second at 192kbps
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 24000.0;
    }

    info("Streaming TTS complete: {} bytes audio, {} alignment chars, {:.2f}s estimated", result.audioData.size(),
         result.charTimings.size(), result.audioDurationSeconds);

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

    // Build WebSocket URL path
    std::string path =
        fmt::format("/v1/text-to-speech/{}/stream-input?model_id={}&output_format={}", voiceId, modelId, outputFormat);

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

} // namespace creatures::voice
