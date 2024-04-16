
#pragma once

#pragma once

#include <string>

#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

#include "server/namespace-stuffs.h"


namespace creatures {

    std::string bytesToString(const std::string& id_bytes);
    std::string animationIdToString(const AnimationId &animationId);
    std::string creatureIdToString(const CreatureId &creature_id);
    std::string playlistIdentifierToString(const PlaylistIdentifier &playlistIdentifier);
    CreatureId stringToCreatureId(const std::string &creatureIdString);
    PlaylistIdentifier stringToPlaylistIdentifier(const std::string &playlistIdString);
    AnimationId stringToAnimationId(const std::string &animationIdString);

    /**
     * Cleanly prints out the things that we're filtering for in an animation list
     *
     * @param filter
     * @return
     */
    std::string animationFilterToString(const AnimationFilter* filter);

    bsoncxx::oid creatureIdToOid(const CreatureId& creature_id);
    bsoncxx::oid animationIdToOid(const AnimationId& animation_id);

    /**
     * Converts the string version of an OID as seen in the MongoDB Compass application, such
     * as `6611dfb4e98d776bb0025304` to a `bsoncxx::oid`
     *
     * @param id_string the OID in string format
     * @return a `bsoncxx::oid` object
     * @throws creatures::InvalidArgumentException if the string is not the correct length or invalid
     */
    bsoncxx::oid stringToOid(const std::string &id_string);

    bsoncxx::oid generateNewOid();

    void displayFrames(const Animation& animation);
    std::string ProtobufTimestampToHumanReadable(const google::protobuf::Timestamp& timestamp);
    google::protobuf::Timestamp time_point_to_protobuf_timestamp(const std::chrono::system_clock::time_point& time_point);

}
