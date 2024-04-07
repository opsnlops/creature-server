
#include <chrono>
#include <ctime>
#include <thread>
#include <iostream>
#include <iomanip>
#include <sstream>

#include <bsoncxx/oid.hpp>
#include <fmt/format.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/util/time_util.h>
#include <grpcpp/grpcpp.h>

#include "server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "client/server-client.h"
#include "util/helpers.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientWriter;

using server::Animation;
using server::AnimationMetadata;
using server::AnimationFilter;
using server::CreatureServer;
using server::Creature;
using server::CreatureFilter;
using server::CreatureId;
using server::CreatureName;
using server::ListAnimationsResponse;
using server::ListCreaturesResponse;
using server::LogItem;
using server::LogLevel;
using server::LogFilter;
using server::Playlist;
using server::PlaylistIdentifier;
using server::PlaylistRequest;
using server::PlaylistResponse;
using server::ListPlaylistsResponse;
using server::PlaylistStatus;
using server::PlaySoundRequest;
using server::PlaySoundResponse;
using server::StreamFrameData;
using server::StreamFrameDataResponse;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;




int main(int argc, char** argv) {

    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_level(spdlog::level::trace);

    // Several tests need the time, let's set it now
    auto currentTime = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(currentTime);

    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    CreatureServerClient client(
            grpc::CreateChannel("127.0.0.1:6666", grpc::InsecureChannelCredentials()));

    info("Searching for a creature name that should exist...");
    Creature reply = client.SearchCreatures("Beaky 🦜");
    info( "client gotten: {}, {}, {}",
          reply.name(), reply.channel_offset(), creatures::ProtobufTimestampToHumanReadable(reply.last_updated()));

    info("Searching for a creature that should NOT exist...");
    reply = client.SearchCreatures("Poop Face");
    trace("doing something with the reply so the compiler doesn't eat it... {}", reply.name());


    // Get the current time as a std::chrono::system_clock::time_point
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    // Convert the time_point to a google::protobuf::Timestamp
    google::protobuf::Timestamp current_timestamp = creatures::time_point_to_protobuf_timestamp(now);

#if 0
    // Let's try to save one
    server::Creature creature = server::Creature();
    creature.set_name("Test Creature from client.cpp");
    creature.set_channel_offset(10);
    creature.set_audio_channel(1);
    creature.set_notes("This is a note! 🐰");
    *creature.mutable_last_updated() = current_timestamp;

    client.CreateCreature(creature);
    info("create done");
#endif

    // Try to get a creature by ID
    std::string oid_string = "6611de58e98d776bb0025301";
    info("attempting to search for creature ID {} in the database...", oid_string);

    bsoncxx::oid oid(oid_string);
    CreatureId creatureId;

    const char* oid_data = oid.bytes();
    creatureId.set__id(oid_data, bsoncxx::oid::k_oid_length);

    reply = client.GetCreature(creatureId);
    info("found creature named {} on a GetCreature call", reply.name());


    // Try to list all the creatures
    info("Now attempting to list all of the creatures!");

    CreatureFilter filter = CreatureFilter();
    filter.set_sortby(::server::SortBy::name);

    auto list = client.ListCreatures(filter);
     for(const auto& id : list.creaturesids() )
     {
         debug("Creature found {}", id.name());
     }


    // Get ALLLLLL of the creatures
    info("Now attempting to get ALLLLLLL of the creatures!");

    filter = CreatureFilter();
    filter.set_sortby(::server::SortBy::name);

    auto everyone = client.GetAllCreatures(filter);
    for(const auto& c : everyone.creatures() )
    {
        debug("Creature found: {}", c.name());
    }

#if 0
    info("Attempting to stream frames");
    client.StreamFrames();
#endif

/* Create Animation Tests */
#if 0
    // Create a simple animation and save it in the database
    Animation animation = Animation();
    Animation_Metadata metadata = Animation_Metadata();
    bsoncxx::oid id;
    animation.set__id(id.to_string());
    metadata.set_title("Example Animation");
    metadata.set_number_of_motors(4);
    metadata.set_milliseconds_per_frame(40);
    metadata.set_creature_type( server::CreatureType::wled_light);
    metadata.set_notes("First note! 🐰");
    metadata.set_sound_file("fartingNoises.flac");
    metadata.set_number_of_frames(10);
    *animation.mutable_metadata() = metadata;


    // Make some frames!
    for(int i = 0; i < 10; i++) {
        auto frame = animation.add_frames();
        for(int j = 0; j < 4; j ++) {
            frame->add_bytes("A");
        }
    }

    client.CreateAnimation(animation);
#endif


/* Creature Update Tests */
#if 0

    std::string unitTestCreatureId = "64717ff45809fc63850d4671";

    // Load the creature
    info("attempting to search for creature ID {} in the database...", unitTestCreatureId);

    CreatureId creatureUpdateId = creatures::stringToCreatureId(unitTestCreatureId);
    Creature creatureToUpdate = client.GetCreature(creatureUpdateId);
    debug("Got creature {}", creatureToUpdate.name());

    // Adjust the name to have a timestamp
    std::ostringstream creatureUpdateTestString;
    creatureUpdateTestString << "Updated at " << std::put_time(std::localtime(&now_time), "%F %T");
    std::string oldName = creatureToUpdate.name();
    std::string newName = creatureUpdateTestString.str();

    creatureToUpdate.set_name(newName);

    // Update in the database
    client.UpdateCreature(creatureToUpdate);

    // Now go fetch it again
    Creature updatedCreature = client.GetCreature(creatureUpdateId);
    info("Update before: {}, now: {}", oldName, updatedCreature.name());

#endif


    // List all the animations for WLED lights
    AnimationFilter animationFilter = AnimationFilter();

    ListAnimationsResponse response = client.ListAnimations(animationFilter);
    for (const auto& a: response.animations())
        info("Found: {}", a.title());

/* Show animation test */
#if 0
    // Attempt to load an animation
    debug("attempting to load an animation");

    std::string animation_oid_string = "64606f237aff7beab00bacb2";
    info("attempting to search for animation ID {} in the database...", animation_oid_string);

    bsoncxx::oid animation_oid(animation_oid_string);
    AnimationId animationId;

    const char* animation_oid_data = animation_oid.bytes();
    animationId.set__id(animation_oid_data, bsoncxx::oid::k_oid_length);

    Animation testAnimation = client.GetAnimation(animationId);
    info("found! Title: {}, number of frames: {}", testAnimation.metadata().title(), testAnimation.frames_size());

    displayFrames(testAnimation);


    // Update the notes on the animation we just loaded
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&now_time), "%F %T");
    std::string str_time = oss.str();

    testAnimation.mutable_metadata()->set_notes(str_time);
    testAnimation.mutable_metadata()->set_title(fmt::format("Test Animation! Updated at {} 😍", str_time));

    info("attempting to the notes on animation {} to {}", animation_oid_string, str_time);
    client.UpdateAnimation(testAnimation);
#endif

    /* Animation Metadata Tests */
#if 0
    // Attempt to load an animationIdentifier
    debug("attempting to load an animationIdentifier");

    std::string animation_oid_string = "64606f237aff7beab00bacb2";
    info("attempting to search for animation ID {} in the database...", animation_oid_string);

    bsoncxx::oid animation_oid(animation_oid_string);
    AnimationId animationId;

    const char* animation_oid_data = animation_oid.bytes();
    animationId.set__id(animation_oid_data, bsoncxx::oid::k_oid_length);

    AnimationMetadata animationMetadata = client.GetAnimationMetadata(animationId);
    info("found! Title: {}", animationMetadata.title());

#endif

#if 0
    // Now let's play animation
    info("attempting to play an animation??");
    std::vector<std::string> creaturesIdsToPlay = {"643ba6ffc606a8b0aa078361"};
    std::string animationPlayTestAnimation = "648534a7a850aa1472070501";

    for(const auto& creatureIdToPlay : creaturesIdsToPlay) {

        bsoncxx::oid aOid(animationPlayTestAnimation);
        AnimationId playbackAnimationId;
        const char *aOiddata = aOid.bytes();
        playbackAnimationId.set__id(aOiddata, bsoncxx::oid::k_oid_length);

        bsoncxx::oid cOid(creatureIdToPlay);
        CreatureId playbackCreatureId;
        const char *cOiddata = cOid.bytes();
        playbackCreatureId.set__id(cOiddata, bsoncxx::oid::k_oid_length);

        PlayAnimationRequest playAnimationRequest;
        *playAnimationRequest.mutable_animationid() = playbackAnimationId;
        *playAnimationRequest.mutable_creatureid() = playbackCreatureId;

        debug("request: {}", playAnimationRequest.DebugString());

        PlayAnimationResponse animationResponse = client.PlayAnimation(playAnimationRequest);

        info(animationResponse.status());
    }
#endif

// Sound Tests
#if 0
    // Try to play a sound

    std::string soundFile = "multi.flac";

    info("attempting to play a sound ({})", soundFile);

    server::PlaySoundRequest soundRequest;
    soundRequest.set_filename(soundFile);

    server::PlaySoundResponse soundResponse = client.PlaySound(soundRequest);

    info("Play response: {}", soundResponse.message());

#endif


    // Playlist tests
#if 0

info("playlists tests!");

    std::ostringstream playlistTestTimestamp;
    playlistTestTimestamp << "Test Playlist: " << std::put_time(std::localtime(&now_time), "%F %T");

    server::Playlist playlist = server::Playlist();
    playlist.set_creature_type(server::CreatureType::parrot);
    playlist.set_name(playlistTestTimestamp.str());

    auto* timestamp = new google::protobuf::Timestamp();
    *timestamp = google::protobuf::util::TimeUtil::GetCurrentTime();
    playlist.set_allocated_last_updated(timestamp);

    // Add ten PlaylistItems
    for (int i = 0; i < 10; i++) {
        Playlist::PlaylistItem* item = playlist.add_items();

        // Create a BSON ObjectID
        bsoncxx::oid itemOid = bsoncxx::oid();

        // Convert the OID to bytes for protobuf
        item->mutable_animationid()->set__id(oid.bytes(), bsoncxx::oid::k_oid_length);
        item->set_weight(i * 10);
    }

    server::DatabaseInfo dbInfo = client.CreatePlaylist(playlist);

    info("server said: {}", dbInfo.message());



    // Attempt to load the playlists for parrots
    PlaylistFilter playlistFilter = PlaylistFilter();
    playlistFilter.set_creature_type(server::CreatureType::parrot);

    auto allPlaylists = client.ListPlaylists(playlistFilter);
    for(const auto& p : allPlaylists.playlists() )
    {
        debug("Playlist found {} with {} items", p.name(), p.items_size());
    }


    // Try to load one playlist
    std::string unitTestPlaylistId = "64d866555ce706669e090278";
    bsoncxx::oid playlistOid(unitTestPlaylistId);
    PlaylistIdentifier playlistId;

    const char* playlist_get_oid_data = playlistOid.bytes();
    playlistId.set__id(playlist_get_oid_data, bsoncxx::oid::k_oid_length);


    Playlist playlistToLoad = client.GetPlaylist(playlistId);
    debug("Playlist found {} with {} items", playlistToLoad.name(), playlistToLoad.items_size());

    for(const auto& item : playlistToLoad.items() )
    {
        debug(" - item {} with weight {}", creatures::animationIdToString(item.animationid()), item.weight());
    }

    // Update a playlist
    Playlist playlistToUpdate = client.GetPlaylist(playlistId);
    std::ostringstream playlistUpdateTestString;
    playlistUpdateTestString << "Unit Test Playlist Updated at " << std::put_time(std::localtime(&now_time), "%F %T");
    std::string playlistOldName = playlistToUpdate.name();
    std::string playlisTNewName = playlistUpdateTestString.str();

    playlistToUpdate.set_name(playlisTNewName);
    playlistToUpdate.set_allocated_last_updated(timestamp);

    client.UpdatePlaylist(playlistToUpdate);
    debug("playlist {} updated from {} to {}", unitTestPlaylistId, playlistOldName, playlisTNewName);


    // Queue up a playlist
    CreatureId creaturePlayTestId = creatures::stringToCreatureId("643b86912a93fc6ba608ba29");
//643b86912a93fc6ba608ba29
    CreaturePlaylistRequest creaturePlaylistRequest;
    *creaturePlaylistRequest.mutable_creatureid() = creaturePlayTestId;
    *creaturePlaylistRequest.mutable_playlistid() = playlistId;
    client.StartPlaylist(creaturePlaylistRequest);


    // Wait 30s, ask for status, sleep 30 more seconds and then stop
    std::this_thread::sleep_for(std::chrono::seconds(30));
    CreaturePlaylistStatus playlistStatus = client.GetPlaylistStatus(creaturePlayTestId);
    info("status: is playing: {}, what: {}", playlistStatus.playing(), creatures::playlistIdentifierToString(playlistStatus.playlistid()));


    std::this_thread::sleep_for(std::chrono::seconds(30));
    info("stopping...");
    client.StopPlaylist(creaturePlayTestId);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    playlistStatus = client.GetPlaylistStatus(creaturePlayTestId);
    info("status: is playing: {}, what: {}", playlistStatus.playing(), creatures::playlistIdentifierToString(playlistStatus.playlistid()));


#endif

    return 0;
}