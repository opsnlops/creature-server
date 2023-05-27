
#pragma once

#include <string>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <chrono>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/timestamp.pb.h>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/array/element.hpp>

#include <bsoncxx/builder/stream/document.hpp>

using server::Creature;
using server::CreatureName;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;


namespace creatures {

    std::string bytesToString(const std::string& id_bytes) {

        std::ostringstream oss;
        for (unsigned char c: id_bytes) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }

        return oss.str();

    }

    std::string creatureIdToString(const CreatureId &creature_id) {
        const std::string &id_bytes = creature_id._id();
        return bytesToString(id_bytes);
    }
}