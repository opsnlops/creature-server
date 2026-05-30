#include "StreamingTTSClient.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include <base64.hpp>
#include <curl/curl.h>
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

// Caps on inbound WebSocket payload size. ElevenLabs frames are typically
// tens of KB (audio chunks) or small JSON; we set a generous per-frame cap
// to reject malicious / corrupted length fields without crashing. The
// fragment cap bounds how much we'll accumulate across continuation
// frames before giving up.
constexpr uint64_t MAX_FRAME_PAYLOAD_BYTES = 16ULL * 1024 * 1024;   // 16 MiB
constexpr uint64_t MAX_FRAGMENT_BUFFER_BYTES = 32ULL * 1024 * 1024; // 32 MiB

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

    // Verify server certificates against the system trust store. Without
    // this, an attacker who can intercept the route to api.elevenlabs.io
    // gets a working MITM and the ElevenLabs API key in the BOS frame
    // flows over their link.
    SSL_CTX_set_min_proto_version(conn_->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(conn_->ctx, SSL_VERIFY_PEER, nullptr);
    if (!SSL_CTX_set_default_verify_paths(conn_->ctx)) {
        std::string msg = "SSL_CTX_set_default_verify_paths failed";
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

    // Tie hostname verification to the cert's subject / SAN
    if (!SSL_set1_host(conn_->ssl, host.c_str())) {
        std::string msg = fmt::format("SSL_set1_host failed for {}", host);
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    if (SSL_connect(conn_->ssl) <= 0) {
        std::string msg = fmt::format("TLS handshake failed with {}", host);
        if (span) {
            span->setError(msg);
        }
        conn_.reset();
        return Result<void>{ServerError(ServerError::InternalError, msg)};
    }

    long verifyResult = SSL_get_verify_result(conn_->ssl);
    if (verifyResult != X509_V_OK) {
        std::string msg =
            fmt::format("TLS cert verification failed for {}: {}", host, X509_verify_cert_error_string(verifyResult));
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
    std::string upgradeRequest = fmt::format("GET {} HTTP/1.1\r\n"
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

Result<StreamingTTSResult>
StreamingTTSClient::receiveAllFrames(const std::string &outputFormat, ProgressCallback progressCallback,
                                     [[maybe_unused]] std::shared_ptr<OperationSpan> parentSpan) {
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
                 "(after {} frames, {} audio bytes)",
                 wsFrameCount, result.audioData.size());
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

        if (payloadLen > MAX_FRAME_PAYLOAD_BYTES) {
            warn("StreamingTTSClient: rejecting frame with payloadLen={} (cap={})", payloadLen,
                 MAX_FRAME_PAYLOAD_BYTES);
            return Result<StreamingTTSResult>{
                ServerError(ServerError::InvalidData, "WebSocket frame exceeded size cap")};
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

        debug("StreamingTTSClient: ws frame #{}: opcode={} fin={} masked={} len={} totalBytes={}", wsFrameCount, opcode,
              fin, masked, payloadLen, totalBytesReceived);

        // Handle fragmentation
        if (opcode == 0x00) {
            if (fragmentBuffer.size() + payload.size() > MAX_FRAGMENT_BUFFER_BYTES) {
                warn("StreamingTTSClient: fragment buffer would exceed cap ({} + {} > {})", fragmentBuffer.size(),
                     payload.size(), MAX_FRAGMENT_BUFFER_BYTES);
                return Result<StreamingTTSResult>{
                    ServerError(ServerError::InvalidData, "WebSocket fragment buffer exceeded size cap")};
            }
            fragmentCount++;
            fragmentBuffer.insert(fragmentBuffer.end(), payload.begin(), payload.end());
            debug("StreamingTTSClient: continuation frame #{}, buffer={} bytes", fragmentCount, fragmentBuffer.size());
            if (!fin) {
                continue;
            }
            opcode = fragmentOpcode;
            payload = std::move(fragmentBuffer);
            fragmentBuffer.clear();
            info("StreamingTTSClient: reassembled {} fragments -> {} bytes (opcode={})", fragmentCount, payload.size(),
                 opcode);
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
                 wsFrameCount, text.size(), textHasAudio, textHasNull, textHasAlign, textHasFinal, text.substr(0, 100));

            try {
                auto json = nlohmann::json::parse(text);

                if (json.contains("isFinal") && json["isFinal"].is_boolean() && json["isFinal"].get<bool>()) {
                    info("StreamingTTSClient: isFinal received after {} frames, {} audio chunks ({} bytes), "
                         "{} alignment chunks ({} chars)",
                         wsFrameCount, audioChunkCount, result.audioData.size(), alignmentChunkCount,
                         result.charTimings.size());
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
                                result.audioData.insert(
                                    result.audioData.end(), reinterpret_cast<const uint8_t *>(decoded.data()),
                                    reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
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
                error("StreamingTTSClient: EXCEPTION in frame #{}: {} size={} preview='{}'", wsFrameCount, e.what(),
                      text.size(), text.substr(0, 200));
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
         wsFrameCount, audioChunkCount, result.audioData.size(), alignmentChunkCount, result.charTimings.size(),
         result.audioDurationSeconds);

    if (result.audioData.empty()) {
        error("StreamingTTSClient: ZERO audio bytes received! Review frame log above.");
    }

    return result;
}

Result<StreamingTTSResult> StreamingTTSClient::generateSpeech(const std::string &apiKey, const std::string &voiceId,
                                                              const std::string &modelId, const std::string &text,
                                                              const std::string &outputFormat, float stability,
                                                              float similarityBoost, ProgressCallback progressCallback,
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
    std::string path =
        fmt::format("/v1/text-to-speech/{}/stream-input?model_id={}&output_format={}&sync_alignment=true", voiceId,
                    modelId, outputFormat);

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

Result<StreamingTTSResult> StreamingTTSClient::generateSpeechREST(const std::string &apiKey, const std::string &voiceId,
                                                                  const std::string &modelId, const std::string &text,
                                                                  const std::string &outputFormat, float stability,
                                                                  float similarityBoost,
                                                                  const std::vector<std::string> &previousRequestIds,
                                                                  ProgressCallback progressCallback,
                                                                  std::shared_ptr<OperationSpan> parentSpan) {

    auto span = creatures::observability->createChildOperationSpan("StreamingTTSClient.generateSpeechREST", parentSpan);
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

    // Build URL
    std::string url =
        fmt::format("https://api.elevenlabs.io/v1/text-to-speech/{}/stream/with-timestamps?output_format={}", voiceId,
                    outputFormat);

    // Result accumulation — curl callbacks append here
    StreamingTTSResult result;
    result.audioFormat = outputFormat;
    int chunkCount = 0;
    std::string lineBuffer; // Accumulates partial lines across curl callbacks

    // Curl write callback: receives response body data
    struct WriteContext {
        StreamingTTSResult *result;
        int *chunkCount;
        std::string *lineBuffer;
    };
    WriteContext writeCtx{&result, &chunkCount, &lineBuffer};

    auto writeCallback = [](char *data, size_t size, size_t nmemb, void *userdata) -> size_t {
        auto *ctx = static_cast<WriteContext *>(userdata);
        size_t totalBytes = size * nmemb;

        // Append to line buffer
        ctx->lineBuffer->append(data, totalBytes);

        // Process all complete newline-delimited JSON lines
        size_t pos = 0;
        while (pos < ctx->lineBuffer->size()) {
            auto nlPos = ctx->lineBuffer->find('\n', pos);
            if (nlPos == std::string::npos) {
                // Partial line — keep remainder for next callback
                *ctx->lineBuffer = ctx->lineBuffer->substr(pos);
                return totalBytes;
            }

            std::string line = ctx->lineBuffer->substr(pos, nlPos - pos);
            pos = nlPos + 1;

            // Trim
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty())
                continue;

            try {
                auto json = nlohmann::json::parse(line);

                // Decode audio
                if (json.contains("audio_base64") && json["audio_base64"].is_string()) {
                    auto audioB64 = json["audio_base64"].get<std::string>();
                    if (!audioB64.empty()) {
                        try {
                            auto decoded = base64::from_base64(audioB64);
                            ctx->result->audioData.insert(
                                ctx->result->audioData.end(), reinterpret_cast<const uint8_t *>(decoded.data()),
                                reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
                            (*ctx->chunkCount)++;
                        } catch (...) {
                            // Base64 decode failed — skip this chunk
                        }
                    }
                }

                // Parse alignment (seconds → milliseconds)
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
                            ct.startTimeMs = starts[i] * 1000.0;
                            ct.durationMs = (ends[i] - starts[i]) * 1000.0;
                            ctx->result->charTimings.push_back(ct);
                            ctx->result->alignmentText.push_back(ct.character);
                        }
                    }
                }
            } catch (const nlohmann::json::exception &) {
                // Not valid JSON — skip
            }
        }

        ctx->lineBuffer->clear();
        return totalBytes;
    };

    // Curl header callback: capture request-id
    struct HeaderContext {
        std::string *requestId;
    };
    HeaderContext headerCtx{&result.requestId};

    auto headerCallback = [](char *data, size_t size, size_t nmemb, void *userdata) -> size_t {
        auto *ctx = static_cast<HeaderContext *>(userdata);
        size_t totalBytes = size * nmemb;
        std::string header(data, totalBytes);

        // Look for request-id header (case-insensitive)
        auto lower = header;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

        if (lower.find("request-id:") == 0 || lower.find("x-request-id:") == 0) {
            auto colonPos = header.find(':');
            if (colonPos != std::string::npos) {
                auto value = header.substr(colonPos + 1);
                // Trim whitespace and newlines
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
                    value.pop_back();
                }
                *ctx->requestId = value;
            }
        }

        return totalBytes;
    };

    if (progressCallback) {
        progressCallback(0.10f);
    }

    // Execute request with curl
    CURL *curl = curl_easy_init();
    if (!curl) {
        std::string msg = "Failed to initialize curl";
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, fmt::format("xi-api-key: {}", apiKey).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     static_cast<size_t (*)(char *, size_t, size_t, void *)>(writeCallback));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                     static_cast<size_t (*)(char *, size_t, size_t, void *)>(headerCallback));
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerCtx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::string msg = fmt::format("ElevenLabs REST TTS curl error: {}", curl_easy_strerror(res));
        error(msg);
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    if (httpCode != 200) {
        std::string msg = fmt::format("ElevenLabs REST TTS HTTP {}", httpCode);
        error(msg);
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    // Process any remaining data in the line buffer
    if (!lineBuffer.empty()) {
        lineBuffer.push_back('\n');
        writeCallback(lineBuffer.data(), 1, lineBuffer.size(), &writeCtx);
    }

    // Estimate duration
    if (outputFormat.find("mp3") != std::string::npos) {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 24000.0;
    } else if (outputFormat.find("pcm") != std::string::npos) {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 88200.0;
    }

    info("StreamingTTSClient REST complete: {} chunks, {} bytes audio, {} alignment chars, "
         "request_id={}, {:.2f}s estimated",
         chunkCount, result.audioData.size(), result.charTimings.size(), result.requestId, result.audioDurationSeconds);

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

// ---------------------------------------------------------------------------
// Phase 1: multi-character dialogue + forced-alignment primitives.
// These are the building blocks for the new /api/v1/animation/dialogue endpoint.
// generateDialogue is intentionally NOT subject to the eleven_v3 blocklist that
// the ad-hoc single-character path enforces — text-to-dialogue is eleven_v3 only.
// ---------------------------------------------------------------------------

std::string StreamingTTSClient::stripTags(const std::string &text) {
    // Drop "[...]" tags, then collapse runs of whitespace to single spaces, then trim.
    std::string noTags;
    noTags.reserve(text.size());
    int depth = 0;
    for (char c : text) {
        if (c == '[') {
            ++depth;
            continue;
        }
        if (c == ']') {
            if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (depth == 0) {
            noTags.push_back(c);
        }
    }

    std::string collapsed;
    collapsed.reserve(noTags.size());
    bool inSpace = false;
    for (char c : noTags) {
        const bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
        if (isSpace) {
            if (!inSpace && !collapsed.empty()) {
                collapsed.push_back(' ');
            }
            inSpace = true;
        } else {
            collapsed.push_back(c);
            inSpace = false;
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

namespace {

/// Common header-callback factory: capture request-id from the response headers
/// and stash it into the given std::string. Same casing rules the existing
/// generateSpeechREST path uses.
size_t captureRequestIdHeader(char *data, size_t size, size_t nmemb, void *userdata) {
    auto *requestId = static_cast<std::string *>(userdata);
    const size_t totalBytes = size * nmemb;
    std::string header(data, totalBytes);

    std::string lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.find("request-id:") == 0 || lower.find("x-request-id:") == 0) {
        const auto colonPos = header.find(':');
        if (colonPos != std::string::npos) {
            std::string value = header.substr(colonPos + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
                value.pop_back();
            }
            *requestId = value;
        }
    }
    return totalBytes;
}

/// Append-to-std::string write callback for one-shot JSON responses.
size_t appendToString(char *data, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<std::string *>(userdata);
    const size_t totalBytes = size * nmemb;
    buf->append(data, totalBytes);
    return totalBytes;
}

} // namespace

Result<DialogueResult> StreamingTTSClient::generateDialogue(const std::string &apiKey,
                                                            const std::vector<DialogueInput> &inputs,
                                                            const std::string &outputFormat,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("StreamingTTSClient.generateDialogue", parentSpan);
    if (span) {
        span->setAttribute("dialogue.inputs", static_cast<int64_t>(inputs.size()));
        span->setAttribute("audio.format", outputFormat);
        std::size_t totalChars = 0;
        for (const auto &in : inputs) {
            totalChars += in.text.size();
        }
        span->setAttribute("dialogue.total_chars", static_cast<int64_t>(totalChars));
    }

    if (inputs.empty()) {
        std::string msg = "generateDialogue requires at least one input turn";
        if (span)
            span->setError(msg);
        return Result<DialogueResult>{ServerError(ServerError::InvalidData, msg)};
    }

    // ---- Build the request body. Text-to-dialogue is eleven_v3-only (other models
    // are rejected by the server with HTTP 400 "does not support dialogue"). The
    // ad-hoc single-character path BLOCKLISTS eleven_v3; we deliberately bypass
    // that here — it's the whole point of this method.
    nlohmann::json body;
    body["model_id"] = "eleven_v3";
    auto inputsArr = nlohmann::json::array();
    for (const auto &in : inputs) {
        inputsArr.push_back({{"voice_id", in.voiceId}, {"text", in.text}});
    }
    body["inputs"] = std::move(inputsArr);
    const std::string bodyStr = body.dump();

    const std::string url =
        fmt::format("https://api.elevenlabs.io/v1/text-to-dialogue/with-timestamps?output_format={}", outputFormat);

    // ---- Execute the request. Single-blob JSON response (NOT newline-delimited),
    // so we accumulate to a string and parse once at the end.
    std::string respBuf;
    DialogueResult result;
    result.audioFormat = outputFormat;

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::string msg = "Failed to initialize curl";
        if (span)
            span->setError(msg);
        return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, fmt::format("xi-api-key: {}", apiKey).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBuf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &captureRequestIdHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result.requestId);
    // Dialogue is slow — eleven_v3 with forced-alignment downstream is the bottleneck.
    // 90s gives headroom for ~2000-char scenes without leaving the call open forever.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::string msg = fmt::format("ElevenLabs dialogue curl error: {}", curl_easy_strerror(res));
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
    }
    if (httpCode != 200) {
        std::string msg = fmt::format("ElevenLabs dialogue HTTP {}: {}", httpCode, respBuf);
        error(msg);
        if (span)
            span->setError(msg);
        // 400 = client problem (bad inputs, unsupported model, char count exceeded);
        // anything else we treat as upstream failure.
        const auto code = (httpCode == 400) ? ServerError::InvalidData : ServerError::InternalError;
        return Result<DialogueResult>{ServerError(code, msg)};
    }

    // ---- Parse the response.
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(respBuf);
    } catch (const nlohmann::json::exception &e) {
        std::string msg = fmt::format("ElevenLabs dialogue: response was not valid JSON: {}", e.what());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
    }

    // audio_base64 → bytes
    if (json.contains("audio_base64") && json["audio_base64"].is_string()) {
        try {
            const auto decoded = base64::from_base64(json["audio_base64"].get<std::string>());
            result.audioData.assign(reinterpret_cast<const uint8_t *>(decoded.data()),
                                    reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
        } catch (const std::exception &e) {
            std::string msg = fmt::format("ElevenLabs dialogue: base64 decode failed: {}", e.what());
            error(msg);
            if (span)
                span->setError(msg);
            return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
        }
    } else {
        std::string msg = "ElevenLabs dialogue: response missing audio_base64";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
    }

    // alignment.characters → kept for downstream sanity checks. The TIMES in
    // alignment are broken on eleven_v3 (confirmed empirically) — we don't expose
    // them on the result and we won't use them. Real timing comes from forcedAlignment().
    if (json.contains("alignment") && json["alignment"].is_object()) {
        const auto &al = json["alignment"];
        if (al.contains("characters") && al["characters"].is_array()) {
            try {
                result.alignmentCharacters = al["characters"].get<std::vector<std::string>>();
            } catch (const nlohmann::json::exception &) {
                // Non-string entries — treat as fatal; the downstream index math relies on this.
                std::string msg = "ElevenLabs dialogue: alignment.characters was not an array of strings";
                error(msg);
                if (span)
                    span->setError(msg);
                return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
            }
        }
    }

    // voice_segments[] — char ranges + speaker. Times kept for diagnostics only.
    if (json.contains("voice_segments") && json["voice_segments"].is_array()) {
        for (const auto &seg : json["voice_segments"]) {
            DialogueVoiceSegment v;
            if (seg.contains("voice_id"))
                v.voiceId = seg["voice_id"].get<std::string>();
            if (seg.contains("character_start_index"))
                v.characterStartIndex = seg["character_start_index"].get<std::size_t>();
            if (seg.contains("character_end_index"))
                v.characterEndIndex = seg["character_end_index"].get<std::size_t>();
            if (seg.contains("dialogue_input_index"))
                v.dialogueInputIndex = seg["dialogue_input_index"].get<std::size_t>();
            if (seg.contains("start_time_seconds"))
                v.startTimeSeconds = seg["start_time_seconds"].get<double>();
            if (seg.contains("end_time_seconds"))
                v.endTimeSeconds = seg["end_time_seconds"].get<double>();
            result.voiceSegments.push_back(std::move(v));
        }
    }
    if (result.voiceSegments.empty()) {
        std::string msg = "ElevenLabs dialogue: response missing voice_segments";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogueResult>{ServerError(ServerError::InternalError, msg)};
    }

    // Duration estimate. pcm_48000 is mono 16-bit @ 48 kHz → 96000 bytes/sec.
    // Generic pcm_* fallback assumes the rate in the format string.
    if (outputFormat == "pcm_48000") {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 96000.0;
    } else if (outputFormat == "pcm_44100") {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 88200.0;
    } else if (outputFormat == "pcm_24000") {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 48000.0;
    } else if (outputFormat.find("mp3") != std::string::npos) {
        // Same heuristic the existing REST path uses; only approximate.
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 24000.0;
    }

    info("Dialogue complete: {} inputs, {} bytes audio, {} alignment chars, {} segments, "
         "~{:.2f}s, request_id={}",
         inputs.size(), result.audioData.size(), result.alignmentCharacters.size(), result.voiceSegments.size(),
         result.audioDurationSeconds, result.requestId);

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(result.audioData.size()));
        span->setAttribute("audio.duration_s", result.audioDurationSeconds);
        span->setAttribute("alignment.chars", static_cast<int64_t>(result.alignmentCharacters.size()));
        span->setAttribute("dialogue.segments", static_cast<int64_t>(result.voiceSegments.size()));
        span->setAttribute("request_id", result.requestId);
        span->setSuccess();
    }
    return result;
}

Result<ForcedAlignmentResult> StreamingTTSClient::forcedAlignment(const std::string &apiKey,
                                                                  const std::vector<uint8_t> &audio,
                                                                  const std::string &contentType,
                                                                  const std::string &transcript,
                                                                  std::shared_ptr<OperationSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("StreamingTTSClient.forcedAlignment", parentSpan);
    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(audio.size()));
        span->setAttribute("audio.content_type", contentType);
        span->setAttribute("transcript.length", static_cast<int64_t>(transcript.size()));
    }

    if (audio.empty()) {
        std::string msg = "forcedAlignment requires non-empty audio";
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InvalidData, msg)};
    }
    if (transcript.empty()) {
        std::string msg = "forcedAlignment requires non-empty transcript";
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InvalidData, msg)};
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::string msg = "Failed to initialize curl";
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }

    // Use curl's MIME API to build the multipart form cleanly.
    curl_mime *mime = curl_mime_init(curl);

    curl_mimepart *filePart = curl_mime_addpart(mime);
    curl_mime_name(filePart, "file");
    curl_mime_filename(filePart, "audio.wav");
    curl_mime_type(filePart, contentType.c_str());
    curl_mime_data(filePart, reinterpret_cast<const char *>(audio.data()), audio.size());

    curl_mimepart *textPart = curl_mime_addpart(mime);
    curl_mime_name(textPart, "text");
    curl_mime_data(textPart, transcript.data(), transcript.size());

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, fmt::format("xi-api-key: {}", apiKey).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string respBuf;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.elevenlabs.io/v1/forced-alignment");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &appendToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::string msg = fmt::format("ElevenLabs forced-alignment curl error: {}", curl_easy_strerror(res));
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }
    if (httpCode != 200) {
        std::string msg = fmt::format("ElevenLabs forced-alignment HTTP {}: {}", httpCode, respBuf);
        error(msg);
        if (span)
            span->setError(msg);
        const auto code = (httpCode == 400) ? ServerError::InvalidData : ServerError::InternalError;
        return Result<ForcedAlignmentResult>{ServerError(code, msg)};
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(respBuf);
    } catch (const nlohmann::json::exception &e) {
        std::string msg = fmt::format("ElevenLabs forced-alignment: response was not valid JSON: {}", e.what());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }

    ForcedAlignmentResult fa;

    auto extractEntries = [](const nlohmann::json &arr, auto &outVec, auto makeEntry) {
        for (const auto &item : arr) {
            if (!item.is_object())
                continue;
            outVec.push_back(makeEntry(item));
        }
    };

    if (json.contains("words") && json["words"].is_array()) {
        extractEntries(json["words"], fa.words, [](const nlohmann::json &it) {
            ForcedAlignmentWord w;
            if (it.contains("text"))
                w.text = it["text"].get<std::string>();
            if (it.contains("start"))
                w.startSeconds = it["start"].get<double>();
            if (it.contains("end"))
                w.endSeconds = it["end"].get<double>();
            return w;
        });
    }
    if (json.contains("characters") && json["characters"].is_array()) {
        extractEntries(json["characters"], fa.characters, [](const nlohmann::json &it) {
            ForcedAlignmentChar c;
            if (it.contains("text"))
                c.text = it["text"].get<std::string>();
            if (it.contains("start"))
                c.startSeconds = it["start"].get<double>();
            if (it.contains("end"))
                c.endSeconds = it["end"].get<double>();
            return c;
        });
    }
    if (json.contains("loss") && json["loss"].is_number()) {
        fa.loss = json["loss"].get<double>();
    }

    if (fa.characters.empty() && fa.words.empty()) {
        std::string msg = "ElevenLabs forced-alignment: response had no words or characters";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }

    info("Forced alignment complete: {} words, {} chars, loss={:.3f}", fa.words.size(), fa.characters.size(), fa.loss);

    if (span) {
        span->setAttribute("forced_alignment.words", static_cast<int64_t>(fa.words.size()));
        span->setAttribute("forced_alignment.chars", static_cast<int64_t>(fa.characters.size()));
        span->setAttribute("forced_alignment.loss", fa.loss);
        span->setSuccess();
    }
    return fa;
}

} // namespace creatures::voice
