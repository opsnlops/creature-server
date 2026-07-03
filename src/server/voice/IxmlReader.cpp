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

} // namespace creatures::voice
