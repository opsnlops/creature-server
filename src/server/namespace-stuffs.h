
// This file is included everywhere, don't declare things in it!

#include "server.pb.h"
#include "spdlog/spdlog.h"

using server::Animation;
using server::AnimationMetadata;
using server::AnimationFilter;
using server::AnimationId;
using server::AnimationIdentifier;
using server::ListAnimationsResponse;
using server::PlayAnimationRequest;
using server::PlayAnimationResponse;


using server::Creature;
using server::CreatureFilter;
using server::CreatureId;
using server::CreatureIdentifier;
using server::CreatureName;
using server::GetAllCreaturesResponse;
using server::ListCreaturesResponse;

using server::DatabaseInfo;

using server::Frame;
using server::StreamFrameDataResponse;

using server::Playlist;
using server::PlaylistFilter;
using server::PlaylistIdentifier;
using server::ListPlaylistsResponse;
using server::CreaturePlaylistResponse;
using server::CreaturePlaylistRequest;
using server::CreaturePlaylistStatus;

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
