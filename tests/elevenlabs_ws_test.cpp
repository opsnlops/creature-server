// Standalone ElevenLabs WebSocket streaming test
// Build: clang++ -std=c++17 -o elevenlabs_ws_test elevenlabs_ws_test.cpp -lssl -lcrypto
// Usage: ./elevenlabs_ws_test

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <openssl/ssl.h>

static const char *API_KEY = "8c275a537e96131c29a878f2b76a6164";
static const char *VOICE_ID = "Nggzl2QAXh3OijoXD116"; // Beaky's voice from the logs
static const char *MODEL_ID = "eleven_turbo_v2_5";
static const char *TEXT = "Hello! This is a quick test of the streaming speech pipeline.";

// --- Base64 encode (for WebSocket key) ---
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64Encode(const uint8_t *data, size_t len) {
    std::string r;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) n |= uint32_t(data[i + 2]);
        r.push_back(b64[(n >> 18) & 0x3F]);
        r.push_back(b64[(n >> 12) & 0x3F]);
        r.push_back((i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=');
        r.push_back((i + 2 < len) ? b64[n & 0x3F] : '=');
    }
    return r;
}

// --- Masked WebSocket frame builder ---
std::vector<uint8_t> buildTextFrame(const std::string &text) {
    std::vector<uint8_t> frame;
    size_t len = text.size();
    frame.push_back(0x81); // FIN + TEXT
    if (len < 126) {
        frame.push_back(uint8_t(0x80 | len));
    } else if (len <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back(uint8_t((len >> 8) & 0xFF));
        frame.push_back(uint8_t(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(uint8_t((len >> (8 * i)) & 0xFF));
    }
    // Masking key
    std::mt19937 rng(std::random_device{}());
    uint8_t mask[4];
    for (auto &m : mask) m = uint8_t(rng() & 0xFF);
    frame.insert(frame.end(), mask, mask + 4);
    // Masked payload
    for (size_t i = 0; i < len; ++i)
        frame.push_back(uint8_t(text[i]) ^ mask[i % 4]);
    return frame;
}

bool sslReadExact(SSL *ssl, uint8_t *buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        int r = SSL_read(ssl, buf + total, int(n - total));
        if (r <= 0) return false;
        total += size_t(r);
    }
    return true;
}

bool sslWriteAll(SSL *ssl, const uint8_t *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        int w = SSL_write(ssl, data + total, int(len - total));
        if (w <= 0) return false;
        total += size_t(w);
    }
    return true;
}

int main() {
    const char *host = "api.elevenlabs.io";
    int port = 443;

    // DNS
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "443", &hints, &res) != 0) {
        std::cerr << "DNS failed\n";
        return 1;
    }

    // TCP connect
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        std::cerr << "TCP connect failed\n";
        return 1;
    }
    freeaddrinfo(res);
    std::cout << "TCP connected\n";

    // TLS — force HTTP/1.1 via ALPN (server defaults to h2 which breaks WS upgrade)
    auto *ctx = SSL_CTX_new(TLS_client_method());
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof(alpn));
    auto *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);
    SSL_set_tlsext_host_name(ssl, host);
    if (SSL_connect(ssl) <= 0) {
        std::cerr << "TLS handshake failed\n";
        return 1;
    }
    std::cout << "TLS established\n";

    // WebSocket upgrade — NO auth header
    std::string path = std::string("/v1/text-to-speech/") + VOICE_ID +
                       "/stream-input?model_id=" + MODEL_ID +
                       "&output_format=mp3_44100_192&sync_alignment=true";

    uint8_t keyBytes[16];
    std::mt19937 rng(std::random_device{}());
    for (auto &b : keyBytes) b = uint8_t(rng() & 0xFF);
    std::string wsKey = base64Encode(keyBytes, 16);

    std::string upgrade =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + wsKey + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    std::cout << "Sending upgrade request (no auth header)...\n";
    sslWriteAll(ssl, (const uint8_t *)upgrade.data(), upgrade.size());

    // Read HTTP response
    std::string response;
    char c;
    int newlines = 0;
    while (true) {
        if (SSL_read(ssl, &c, 1) <= 0) break;
        response.push_back(c);
        if (c == '\n') { if (newlines == 1) break; newlines++; }
        else if (c != '\r') newlines = 0;
    }

    std::cout << "Response:\n" << response << "\n";

    if (response.find("101") == std::string::npos) {
        std::cerr << "Upgrade FAILED (not 101)\n";

        // Try again but with the xi-api-key header this time
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(sock);

        // Reconnect
        getaddrinfo(host, "443", &hints, &res);
        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        connect(sock, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        ctx = SSL_CTX_new(TLS_client_method());
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, host);
        SSL_connect(ssl);

        std::cout << "\n--- Retry WITH xi-api-key header ---\n";
        for (auto &b : keyBytes) b = uint8_t(rng() & 0xFF);
        wsKey = base64Encode(keyBytes, 16);

        upgrade =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + wsKey + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "xi-api-key: " + API_KEY + "\r\n"
            "\r\n";

        sslWriteAll(ssl, (const uint8_t *)upgrade.data(), upgrade.size());

        response.clear();
        newlines = 0;
        while (true) {
            if (SSL_read(ssl, &c, 1) <= 0) break;
            response.push_back(c);
            if (c == '\n') { if (newlines == 1) break; newlines++; }
            else if (c != '\r') newlines = 0;
        }

        std::cout << "Response:\n" << response << "\n";

        if (response.find("101") == std::string::npos) {
            std::cerr << "Both approaches failed. Exiting.\n";
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(sock);
            return 1;
        }
    }

    std::cout << "WebSocket connected!\n";

    // Send BOS with xi_api_key in body
    std::string bos = R"({"text":" ","voice_settings":{"stability":0.3,"similarity_boost":0.72},"xi_api_key":")" +
                      std::string(API_KEY) + R"(","try_trigger_generation":false})";
    std::cout << "Sending BOS: " << bos << "\n";
    auto bosFrame = buildTextFrame(bos);
    sslWriteAll(ssl, bosFrame.data(), bosFrame.size());

    // Send text
    std::string textMsg = R"({"text":")" + std::string(TEXT) + R"(","try_trigger_generation":true})";
    std::cout << "Sending text: " << textMsg << "\n";
    auto textFrame = buildTextFrame(textMsg);
    sslWriteAll(ssl, textFrame.data(), textFrame.size());

    // Send EOS
    std::string eos = R"({"text":""})";
    auto eosFrame = buildTextFrame(eos);
    sslWriteAll(ssl, eosFrame.data(), eosFrame.size());
    std::cout << "Sent EOS\n";

    // Receive frames
    size_t totalAudio = 0;
    size_t totalAlignment = 0;
    int frameCount = 0;

    while (true) {
        uint8_t hdr[2];
        if (!sslReadExact(ssl, hdr, 2)) {
            std::cout << "Connection closed\n";
            break;
        }

        uint8_t opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t payloadLen = hdr[1] & 0x7F;

        if (payloadLen == 126) {
            uint8_t ext[2];
            sslReadExact(ssl, ext, 2);
            payloadLen = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (payloadLen == 127) {
            uint8_t ext[8];
            sslReadExact(ssl, ext, 8);
            payloadLen = 0;
            for (int i = 0; i < 8; ++i) payloadLen = (payloadLen << 8) | ext[i];
        }

        uint8_t mask[4] = {};
        if (masked) sslReadExact(ssl, mask, 4);

        std::vector<uint8_t> payload(payloadLen);
        if (payloadLen > 0) {
            if (!sslReadExact(ssl, payload.data(), payloadLen)) break;
            if (masked)
                for (size_t i = 0; i < payloadLen; ++i) payload[i] ^= mask[i % 4];
        }

        frameCount++;

        if (opcode == 0x01) { // Text
            std::string text(payload.begin(), payload.end());
            // Check for audio field
            if (text.find("\"audio\"") != std::string::npos && text.find("\"isFinal\"") == std::string::npos) {
                // Count rough audio size
                auto audioStart = text.find("\"audio\":\"");
                if (audioStart != std::string::npos) {
                    audioStart += 9;
                    auto audioEnd = text.find("\"", audioStart);
                    if (audioEnd != std::string::npos) {
                        totalAudio += (audioEnd - audioStart) * 3 / 4; // rough b64 decode size
                    }
                }
                if (text.find("\"alignment\"") != std::string::npos) {
                    totalAlignment++;
                }
                std::cout << "  chunk " << frameCount << ": ~" << (payloadLen / 1024) << "KB json (audio+alignment)\n";
            } else {
                std::cout << "  msg " << frameCount << ": " << text.substr(0, 200) << "\n";
            }

            if (text.find("\"isFinal\"") != std::string::npos) {
                std::cout << "Got isFinal!\n";
                break;
            }
        } else if (opcode == 0x02) {
            totalAudio += payloadLen;
            std::cout << "  binary frame " << frameCount << ": " << payloadLen << " bytes\n";
        } else if (opcode == 0x08) {
            std::cout << "Close frame received\n";
            break;
        } else if (opcode == 0x09) {
            // Pong
            auto pong = buildTextFrame("");
            pong[0] = 0x8A; // FIN + PONG
            sslWriteAll(ssl, pong.data(), pong.size());
        }
    }

    std::cout << "\n=== Results ===\n";
    std::cout << "Frames received: " << frameCount << "\n";
    std::cout << "Total audio (approx): " << totalAudio << " bytes\n";
    std::cout << "Alignment chunks: " << totalAlignment << "\n";

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sock);
    return 0;
}
