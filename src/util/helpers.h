
#pragma once

#pragma once

#include <string>
#include "server/namespace-stuffs.h"


namespace creatures {

    std::string bytesToString(const std::string& id_bytes);
    std::string creatureIdToString(const CreatureId &creature_id);
    std::string playlistIdentifierToString(const PlaylistIdentifier &playlistIdentifier);
}
