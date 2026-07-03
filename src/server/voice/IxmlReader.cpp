#include "server/voice/IxmlReader.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace creatures::voice {

namespace {

template <typename T> bool readLE(std::ifstream &in, T &out) {
    in.read(reinterpret_cast<char *>(&out), sizeof(T));
    return in.good();
}

} // namespace

std::optional<std::string> readIxmlChunk(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    char riff[4]{};
    uint32_t riffSize = 0;
    char wave[4]{};
    in.read(riff, 4);
    if (!readLE(in, riffSize)) {
        return std::nullopt;
    }
    in.read(wave, 4);
    if (!in.good() || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return std::nullopt;
    }

    // Walk subchunks looking for iXML, skipping every other chunk's payload
    // (including the large `data` chunk) by its declared size.
    while (in.good()) {
        char chunkId[4]{};
        uint32_t chunkSize = 0;
        in.read(chunkId, 4);
        if (!readLE(in, chunkSize)) {
            break;
        }

        if (std::memcmp(chunkId, "iXML", 4) == 0) {
            std::vector<char> payload(chunkSize);
            if (chunkSize > 0) {
                in.read(payload.data(), static_cast<std::streamsize>(chunkSize));
                if (in.gcount() != static_cast<std::streamsize>(chunkSize)) {
                    return std::nullopt; // truncated chunk
                }
            }
            return std::string(payload.begin(), payload.end());
        }

        // Skip this chunk's payload (+1 if odd-sized, for RIFF word alignment).
        std::streamoff skip = static_cast<std::streamoff>(chunkSize);
        if (chunkSize % 2 == 1) {
            skip += 1;
        }
        in.seekg(skip, std::ios::cur);
    }

    return std::nullopt;
}

namespace {

// Reverse of IxmlWriter's xmlEscape. Handles exactly the five entities we emit.
std::string xmlUnescape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == '&') {
            if (in.compare(i, 5, "&amp;") == 0) {
                out += '&';
                i += 5;
                continue;
            }
            if (in.compare(i, 4, "&lt;") == 0) {
                out += '<';
                i += 4;
                continue;
            }
            if (in.compare(i, 4, "&gt;") == 0) {
                out += '>';
                i += 4;
                continue;
            }
            if (in.compare(i, 6, "&quot;") == 0) {
                out += '"';
                i += 6;
                continue;
            }
            if (in.compare(i, 6, "&apos;") == 0) {
                out += '\'';
                i += 6;
                continue;
            }
        }
        out += in[i];
        ++i;
    }
    return out;
}

} // namespace

std::optional<std::string> extractIxmlField(const std::string &ixmlDocument, const std::string &tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const auto start = ixmlDocument.find(open);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const auto contentStart = start + open.size();
    const auto end = ixmlDocument.find(close, contentStart);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return xmlUnescape(ixmlDocument.substr(contentStart, end - contentStart));
}

} // namespace creatures::voice
