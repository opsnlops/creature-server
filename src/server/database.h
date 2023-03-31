
#pragma once

#include <string>

// TODO: Clean this up
#define DB_URI  "mongodb://10.3.2.11"

using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;


#include "messaging/server.pb.h"

namespace creatures {

    class Database {

    public:
        Database();

        int saveCreature(Creature);
        Creature getCreature(CreatureName);


    };


}
