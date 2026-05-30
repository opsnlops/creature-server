
// This file is included everywhere, don't declare things in it!

#include "spdlog/spdlog.h"

using spdlog::critical;
using spdlog::debug;
using spdlog::error;
using spdlog::info;
using spdlog::trace;
using spdlog::warn;

using universe_t = uint32_t;
using framenum_t = uint64_t;

using creatureId_t = std::string;
using animationId_t = std::string;
using playlistId_t = std::string;
using fixtureId_t = std::string;
using scriptId_t = std::string;
