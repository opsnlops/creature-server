

#include <string>
#include <iomanip>
#include <sstream>


#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>



#include "exception/exception.h"
#include "server/namespace-stuffs.h"

namespace creatures {

    std::string bytesToString(const std::string& id_bytes) {

        std::ostringstream oss;
        for (unsigned char c: id_bytes) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }

        return oss.str();

    }

    std::string animationIdToString(const AnimationId &animationId) {
        const std::string &id_bytes = animationId._id();
        return bytesToString(id_bytes);
    }

    std::string creatureIdToString(const CreatureId &creature_id) {
        const std::string &id_bytes = creature_id._id();
        return bytesToString(id_bytes);
    }

    std::string playlistIdentifierToString(const PlaylistIdentifier &playlistIdentifier) {
        const std::string &id_bytes = playlistIdentifier._id();
        return bytesToString(id_bytes);
    }

    CreatureId stringToCreatureId(const std::string &creatureIdString) {
        bsoncxx::oid creatureIdOid(creatureIdString);
        CreatureId creatureId;

        const char* creatureIdOidData = creatureIdOid.bytes();
        creatureId.set__id(creatureIdOidData, bsoncxx::oid::k_oid_length);

        return creatureId;
    }

    PlaylistIdentifier stringToPlaylistIdentifier(const std::string &playlistIdString) {
        bsoncxx::oid playlistIdOid(playlistIdString);
        PlaylistIdentifier playlistIdentifier;

        const char* playlistIdOidData = playlistIdOid.bytes();
        playlistIdentifier.set__id(playlistIdOidData, bsoncxx::oid::k_oid_length);

        return playlistIdentifier;
    }

    AnimationId stringToAnimationId(const std::string &animationIdString) {
        bsoncxx::oid animationIdOid(animationIdString);
        AnimationId animationId;

        const char* animationIdOidData = animationIdOid.bytes();
        animationId.set__id(animationIdOidData, bsoncxx::oid::k_oid_length);

        return animationId;
    }

    /**
     * Creates a new oid. Trivial, but important that I do it the same way always.
     *
     * @return a shiny new oid
     */
    bsoncxx::oid generateNewOid() {
        return bsoncxx::oid{};
    }

    /**
     * Converts the string version of an OID as seen in the MongoDB Compass application, such
     * as `6611dfb4e98d776bb0025304` to a `bsoncxx::oid`
     *
     * @param id_string the OID in string format
     * @return a `bsoncxx::oid` object
     * @throws creatures::InvalidArgumentException if the string is not the correct length or invalid
     */
    bsoncxx::oid stringToOid(const std::string &id_string) {

        const size_t expected_length = bsoncxx::oid::k_oid_length * 2; // 12 bytes * 2 characters per byte

        // Make sure it's the right length
        if (id_string.size() != expected_length) {
            warn("String is not the right length for an ObjectId: {}", id_string);
            std::string errorMessage = fmt::format("String is not the right length for an ObjectId. (Given: {}, Expected: {})",
                                                   id_string.size(), expected_length);
            throw creatures::InvalidArgumentException(errorMessage);
        }

        try {

            /*
             * The string that's given will be in UTF-8. There's actually only four bits per character
             * in hex, so we need to convert the UTF-8 string into a binary string. We'll do this by
             * converting each pair of characters into a byte.
             */

            std::string bytes;
            bytes.reserve(bsoncxx::oid::k_oid_length);
            for (size_t i = 0; i < expected_length; i += 2) {
                std::string byteString = id_string.substr(i, 2);
                bytes.push_back(static_cast<char>(std::stoul(byteString, nullptr, 16)));
            }

            return bsoncxx::oid(bytes.data(), bytes.size());
        }
        catch (const std::exception &e) {
            warn("Error converting string to ObjectId: {}", e.what());
            throw creatures::InvalidArgumentException("Error converting string to ObjectId: " + std::string(e.what()));
        }
    }

    void displayFrames(const server::FrameData& animation) {
        int frameCounter = 0;
        for (const auto& frame : animation.frames()) {
            std::ostringstream byteStream;
            byteStream << std::hex << std::setfill('0');
            for (unsigned char byte : frame) { // Directly iterate over the bytes in the frame
                byteStream << std::setw(2) << static_cast<int>(byte) << " ";
            }

            std::cout << "Frame " << frameCounter << ": [" << byteStream.str() << "]" << std::endl;
            frameCounter++;
        }
    }

    std::string animationFilterToString(const AnimationFilter* filter) {
        std::ostringstream oss;
        oss << "AnimationFilter{ creatureId=" << creatureIdToString(filter->creature_id()) << " }";
        return oss.str();
    }

    bsoncxx::oid creatureIdToOid(const CreatureId& creature_id) {
        // Error checking
        if (creature_id._id().size() != bsoncxx::oid::k_oid_length) {
            throw std::runtime_error("Invalid ObjectId size.");
        }

        return bsoncxx::oid(creature_id._id().data(), bsoncxx::oid::k_oid_length);
    }

    // This is super simple, but easier for me to read!
    std::string oidToString(const bsoncxx::oid& oid) {
        return oid.to_string();
    }

    bsoncxx::oid animationIdToOid(const AnimationId& animation_id) {
          if (animation_id._id().size() != bsoncxx::oid::k_oid_length) {
            throw std::runtime_error("Invalid ObjectId size.");
        }

        return bsoncxx::oid(animation_id._id().data(), bsoncxx::oid::k_oid_length);
    }


    // Mostly used by FrameData
    bsoncxx::document::value stringVectorToBson(const std::vector<std::string> &vector);
    std::vector<std::string> stringVectorFromBson(const bsoncxx::document::view &doc);


    std::string ProtobufTimestampToHumanReadable(const google::protobuf::Timestamp& timestamp) {
        // Combine seconds and nanoseconds into a single duration
        auto seconds_duration = std::chrono::seconds(timestamp.seconds());
        auto nanoseconds_duration = std::chrono::nanoseconds(timestamp.nanos());

        // Convert the duration to a std::chrono::system_clock::time_point
        std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(0) + seconds_duration;
        tp += std::chrono::duration_cast<std::chrono::system_clock::duration>(nanoseconds_duration);

        // Convert the std::chrono::system_clock::time_point to a std::time_t
        std::time_t time = std::chrono::system_clock::to_time_t(tp);

        // Convert the std::time_t to a std::tm
        std::tm tm{};
        gmtime_r(&time, &tm);

        // Format the std::tm into a human-readable string
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

        // Return the formatted string
        return {buffer};
    }



    google::protobuf::Timestamp time_point_to_protobuf_timestamp(const std::chrono::system_clock::time_point& time_point) {
        google::protobuf::Timestamp timestamp;
        auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch());
        auto nanos_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch());
        timestamp.set_seconds(seconds_since_epoch.count());
        timestamp.set_nanos(nanos_since_epoch.count() % 1000000000); // Only store nanoseconds part
        return timestamp;
    }


}
