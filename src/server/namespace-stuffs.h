
// This file is included everywhere, don't declare things in it!

#include "server.pb.h"
#include "spdlog/spdlog.h"



using server::Animation;
using server::Animation_Metadata;
using server::AnimationFilter;
using server::AnimationId;
using server::ListAnimationsResponse;
using server::PlayAnimationRequest;
using server::PlayAnimationResponse;


using server::Creature;
using server::CreatureFilter;
using server::CreatureId;
using server::CreatureIdentifier;
using server::CreatureName;
using server::CreatureType;
using server::GetAllCreaturesResponse;
using server::ListCreaturesResponse;

using server::DatabaseInfo;

using server::Frame;
using server::FrameResponse;

using server::PlaySoundRequest;
using server::PlaySoundResponse;

using server::LogFilter;
using server::LogItem;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;
