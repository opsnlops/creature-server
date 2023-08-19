

#include <string>

#include <iomanip>
#include <sstream>

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
}
