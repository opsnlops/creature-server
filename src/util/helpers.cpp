

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

#include <base64.hpp>

#include "exception/exception.h"
#include "server/namespace-stuffs.h"

namespace fs = std::filesystem;

namespace creatures {

std::string bytesToString(const std::string &id_bytes) {

    trace("converting {} bytes to a string", id_bytes.size());

    std::ostringstream oss;
    for (unsigned char c : id_bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(c);
    }

    debug("converted bytes to string: {}", oss.str());
    return oss.str();
}

//    std::string animationIdToString(const AnimationId &animationId) {
//        const std::string &id_bytes = animationId._id();
//        return bytesToString(id_bytes);
//    }
//
//    std::string creatureIdToString(const CreatureId &creature_id) {
//        const std::string &id_bytes = creature_id._id();
//        return bytesToString(id_bytes);
//    }
//
//    std::string playlistIdentifierToString(const PlaylistIdentifier
//    &playlistIdentifier) {
//        const std::string &id_bytes = playlistIdentifier._id();
//        return bytesToString(id_bytes);
//    }
//
//    CreatureId stringToCreatureId(const std::string &creatureIdString) {
//        bsoncxx::oid creatureIdOid(creatureIdString);
//        CreatureId creatureId;
//
//        const char* creatureIdOidData = creatureIdOid.bytes();
//        creatureId.set__id(creatureIdOidData, bsoncxx::oid::k_oid_length);
//
//        return creatureId;
//    }
//
//    PlaylistIdentifier stringToPlaylistIdentifier(const std::string
//    &playlistIdString) {
//        bsoncxx::oid playlistIdOid(playlistIdString);
//        PlaylistIdentifier playlistIdentifier;
//
//        const char* playlistIdOidData = playlistIdOid.bytes();
//        playlistIdentifier.set__id(playlistIdOidData,
//        bsoncxx::oid::k_oid_length);
//
//        return playlistIdentifier;
//    }
//
//    AnimationId stringToAnimationId(const std::string &animationIdString) {
//        bsoncxx::oid animationIdOid(animationIdString);
//        AnimationId animationId;
//
//        const char* animationIdOidData = animationIdOid.bytes();
//        animationId.set__id(animationIdOidData, bsoncxx::oid::k_oid_length);
//
//        return animationId;
//    }

/**
 * Creates a new oid. Trivial, but important that I do it the same way always.
 *
 * @return a shiny new oid
 */
bsoncxx::oid generateNewOid() { return bsoncxx::oid{}; }

/**
 * Converts the string version of an OID as seen in the MongoDB Compass
 * application, such as `6611dfb4e98d776bb0025304` to a `bsoncxx::oid`
 *
 * @param id_string the OID in string format
 * @return a `bsoncxx::oid` object
 * @throws creatures::InvalidArgumentException if the string is not the correct
 * length or invalid
 */
bsoncxx::oid stringToOid(const std::string &id_string) {

    const size_t expected_length =
        bsoncxx::oid::k_oid_length * 2; // 12 bytes * 2 characters per byte

    // Make sure it's the right length
    if (id_string.size() != expected_length) {
        warn("String is not the right length for an ObjectId: {}", id_string);
        std::string errorMessage =
            fmt::format("String is not the right length for an ObjectId. "
                        "(Given: {}, Expected: {})",
                        id_string.size(), expected_length);
        throw creatures::InvalidArgumentException(errorMessage);
    }

    try {

        /*
         * The string that's given will be in UTF-8. There's actually only four
         * bits per character in hex, so we need to convert the UTF-8 string
         * into a binary string. We'll do this by converting each pair of
         * characters into a byte.
         */

        std::string bytes;
        bytes.reserve(bsoncxx::oid::k_oid_length);
        for (size_t i = 0; i < expected_length; i += 2) {
            std::string byteString = id_string.substr(i, 2);
            bytes.push_back(
                static_cast<char>(std::stoul(byteString, nullptr, 16)));
        }

        return bsoncxx::oid(bytes.data(), bytes.size());
    } catch (const std::exception &e) {
        warn("Error converting string to ObjectId: {}", e.what());
        throw creatures::InvalidArgumentException(
            "Error converting string to ObjectId: " + std::string(e.what()));
    }
}

//    void displayFrames(const server::FrameData& animation) {
//        int frameCounter = 0;
//        for (const auto& frame : animation.frames()) {
//            std::ostringstream byteStream;
//            byteStream << std::hex << std::setfill('0');
//            for (unsigned char byte : frame) { // Directly iterate over the
//            bytes in the frame
//                byteStream << std::setw(2) << static_cast<int>(byte) << " ";
//            }
//
//            std::cout << "Frame " << frameCounter << ": [" << byteStream.str()
//            << "]" << std::endl; frameCounter++;
//        }
//    }
//
//    std::string animationFilterToString(const AnimationFilter* filter) {
//        std::ostringstream oss;
//        oss << "AnimationFilter{ creatureId=" <<
//        creatureIdToString(filter->creature_id()) << " }"; return oss.str();
//    }
//
//    bsoncxx::oid creatureIdToOid(const CreatureId& creature_id) {
//        // Error checking
//        if (creature_id._id().size() != bsoncxx::oid::k_oid_length) {
//            throw std::runtime_error("Invalid ObjectId size.");
//        }
//
//        return bsoncxx::oid(creature_id._id().data(),
//        bsoncxx::oid::k_oid_length);
//    }

// This is super simple, but easier for me to read!
std::string oidToString(const bsoncxx::oid &oid) { return oid.to_string(); }

//    bsoncxx::oid animationIdToOid(const AnimationId& animation_id) {
//          if (animation_id._id().size() != bsoncxx::oid::k_oid_length) {
//            throw std::runtime_error("Invalid ObjectId size.");
//        }
//
//        return bsoncxx::oid(animation_id._id().data(),
//        bsoncxx::oid::k_oid_length);
//    }

bsoncxx::document::value
stringVectorToBson(const std::vector<std::string> &vector) {
    (void)vector;
    throw InternalError("Not implemented");
}
std::vector<std::string>
stringVectorFromBson(const bsoncxx::document::view &doc) {
    (void)doc;
    throw InternalError("Not implemented");
}

bool fileIsReadable(const std::string &path) {

    debug("checking to see if file is readable: {}", path);

    fs::path file_path = path;

    // Check if the file exists and has read permissions
    if (fs::exists(file_path) && fs::is_regular_file(file_path)) {
        std::error_code ec;

        // Try to open the file
        std::ifstream file(file_path, std::ios::in);
        if (file) {
            file.close();
            debug("file is readable");
            return true;
        }
    }

    debug("file is not readable");
    return false;
}

std::string getCurrentTimeISO8601() {
    // Get the current time point from the system clock
    auto now = std::chrono::system_clock::now();
    auto time_now = std::chrono::system_clock::to_time_t(now);

    // Create a stringstream to store the formatted time
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_now), "%Y-%m-%dT%H:%M:%SZ");

    return ss.str();
}

std::string formatTimeISO8601(std::chrono::system_clock::time_point timePoint) {
    auto timeT = std::chrono::system_clock::to_time_t(timePoint);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&timeT), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::vector<uint8_t> decodeBase64(const std::string &base64Data) {

    // Decode the base64 string to raw bytes
    std::string decodedString = base64::from_base64(base64Data);

    // Convert the decoded string to a std::vector<uint8_t>
    std::vector<uint8_t> result(decodedString.begin(), decodedString.end());

    return result;
}

std::string vectorToHexString(const std::vector<uint8_t> &byteVector) {
    std::ostringstream oss;
    oss << "[ ";

    for (size_t i = 0; i < byteVector.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << "0x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(byteVector[i]);
    }

    oss << " ]";
    return oss.str();
}
} // namespace creatures
