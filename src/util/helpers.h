
#pragma once

#pragma once

#include <string>
#include "server/namespace-stuffs.h"


namespace creatures {

    std::string bytesToString(const std::string& id_bytes);
    std::string animationIdToString(const AnimationId &animationId);
    std::string creatureIdToString(const CreatureId &creature_id);
    std::string playlistIdentifierToString(const PlaylistIdentifier &playlistIdentifier);

    void displayFrames(const Animation& animation);
    std::string ProtobufTimestampToHumanReadable(const google::protobuf::Timestamp& timestamp);
    google::protobuf::Timestamp time_point_to_protobuf_timestamp(const std::chrono::system_clock::time_point& time_point);

}
