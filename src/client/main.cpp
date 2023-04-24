
#include <chrono>
#include <ctime>
#include <thread>

#include <bsoncxx/oid.hpp>
#include <fmt/format.h>
#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/grpcpp.h>

#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "client/server-client.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientWriter;

using server::Animation;
using server::Animation_Metadata;
using server::AnimationFilter;
using server::AnimationIdentifier;
using server::CreatureServer;
using server::Creature;
using server::CreatureFilter;
using server::CreatureId;
using server::CreatureName;
using server::Frame;
using server::FrameResponse;
using server::ListAnimationsResponse;
using server::ListCreaturesResponse;
using server::LogItem;
using server::LogLevel;
using server::LogFilter;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

std::string ProtobufTimestampToHumanReadable(const google::protobuf::Timestamp& timestamp) {
    // Combine seconds and nanoseconds into a single duration
    auto seconds_duration = std::chrono::seconds(timestamp.seconds());
    auto nanoseconds_duration = std::chrono::nanoseconds(timestamp.nanos());

    // Convert the duration to a std::chrono::system_clock::time_point
    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(0) + seconds_duration;
    tp += std::chrono::duration_cast<std::chrono::system_clock::duration>(nanoseconds_duration);

    // Convert the std::chrono::system_clock::time_point to a std::time_t
    std::time_t time = std::chrono::system_clock::to_time_t(tp);

    // Convert the std::time_t to a std::tm
    std::tm tm{};
    gmtime_r(&time, &tm);

    // Format the std::tm into a human-readable string
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

    // Return the formatted string
    return {buffer};
}



google::protobuf::Timestamp time_point_to_protobuf_timestamp(const std::chrono::system_clock::time_point& time_point) {
    google::protobuf::Timestamp timestamp;
    auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch());
    auto nanos_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch());
    timestamp.set_seconds(seconds_since_epoch.count());
    timestamp.set_nanos(nanos_since_epoch.count() % 1000000000); // Only store nanoseconds part
    return timestamp;
}




int main(int argc, char** argv) {

    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_level(spdlog::level::trace);

    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    CreatureServerClient client(
            grpc::CreateChannel("localhost:6666", grpc::InsecureChannelCredentials()));

    info("Searching for a creature name that should exist...");
    Creature reply = client.SearchCreatures("Beaky1");
    info( "client gotten: {}, {}, {}, {}",
          reply.name(), reply.sacn_ip(), reply.dmx_base(), ProtobufTimestampToHumanReadable(reply.last_updated()));

    info("Searching for a creature that should NOT exist...");
    reply = client.SearchCreatures("Poop Face");
    trace("doing something with the reply so the compiler doesn't eat it... {}", reply.name());


    // Get the current time as a std::chrono::system_clock::time_point
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    // Convert the time_point to a google::protobuf::Timestamp
    google::protobuf::Timestamp current_timestamp = time_point_to_protobuf_timestamp(now);


    // Let's try to save one
    server::Creature creature = server::Creature();
    creature.set_name("Beaky8");
    creature.set_dmx_base(1);
    creature.set_number_of_motors(7);
    creature.set_universe(1);
    creature.set_sacn_ip("10.3.2.11");
    *creature.mutable_last_updated() = current_timestamp;

    for(int i = 0; i < 7; i++) {
        ::Creature::Motor *motor = creature.add_motors();

        motor->set_name(fmt::format("Motor {} 🦾", i));      // Toss in some UTF-8 for testing
        motor->set_min_value(253);
        motor->set_max_value(2934);
        motor->set_smoothing_value(0.932f);
        motor->set_number(i);

        if(i % 2 == 0)
            motor->set_type(::Creature::servo);
        else
            motor->set_type(::Creature::stepper);
    }

    client.CreateCreature(creature);
    info("create done");


    // Try to get a creature by ID
    std::string oid_string = "6431c48d6e9cc35e2d089263";
    info("attempting to search for ID {} in the database...", oid_string);

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
        debug("Creature found {} with {} motors", c.name(), c.number_of_motors());
    }


    info("Attempting to stream frames");
    client.StreamFrames();


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



    // List all the animations for WLED lights
    AnimationFilter animationFilter = AnimationFilter();
    animationFilter.set_type(server::CreatureType::wled_light);

    ListAnimationsResponse response = client.ListAnimations(animationFilter);
    for (const auto& a: response.animations())
        info("Found: {}", a.metadata().title());



    // Attempt to load an animation
    debug("attempting to load an animation");

    std::string animation_oid_string = "6445ef7e71727101ee0239c7";
    info("attempting to search for animation ID {} in the database...", animation_oid_string);

    bsoncxx::oid animation_oid(animation_oid_string);
    AnimationId animationId;

    const char* animation_oid_data = animation_oid.bytes();
    animationId.set__id(animation_oid_data, bsoncxx::oid::k_oid_length);

    Animation testAnimation = client.GetAnimation(animationId);
    info("found! Title: {}, number of frames: {}", testAnimation.metadata().title(), testAnimation.frames_size());

    for( const auto& f : testAnimation.frames()) {

        debug("dumping frame...");
        debug(f.DebugString());
    }

    return 0;
}