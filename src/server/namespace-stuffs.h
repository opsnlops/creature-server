
// This file is included everywhere, don't declare things in it!

#include "server.pb.h"
#include "spdlog/spdlog.h"

using server::Animation;
using server::AnimationMetadata;
using server::AnimationFilter;
using server::AnimationId;
using server::ListAnimationsResponse;
using server::PlayAnimationRequest;
using server::PlayAnimationResponse;
using server::FrameData;


using server::CreatureFilter;
using server::CreatureId;
using server::CreatureIdentifier;
using server::CreatureName;
using server::GetAllCreaturesResponse;
using server::ListCreaturesResponse;

using server::DatabaseInfo;

//using server::Frame;
using server::StreamFrameDataResponse;
using server::StreamFrameData;

using server::Playlist;
using server::PlaylistFilter;
using server::PlaylistIdentifier;
using server::ListPlaylistsResponse;
using server::PlaylistResponse;
using server::PlaylistRequest;
using server::PlaylistStatus;
using server::PlaylistStopRequest;

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


using universe_t = uint32_t;
using framenum_t = uint64_t;
