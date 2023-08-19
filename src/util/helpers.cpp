

#include <string>
#include <iomanip>
#include <sstream>

#include <bsoncxx/oid.hpp>

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


    void displayFrames(const Animation& animation) {
        int frameCounter = 0;
        for (const auto& frame : animation.frames()) {
            std::ostringstream byteStream;
            byteStream << std::hex << std::setfill('0');
            for (const auto& byteBlock : frame.bytes()) {
                for (unsigned char byte : byteBlock) {
                    byteStream << std::setw(2) << static_cast<int>(byte) << " ";
                }
            }

            std::cout << "Frame " << frameCounter << ": [ " << byteStream.str() << "]" << std::endl;
            frameCounter++;
        }
    }

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
