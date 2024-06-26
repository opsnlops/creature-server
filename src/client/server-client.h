
#pragma once

#include <chrono>
#include <ctime>
#include <memory>
#include <sstream>
#include <thread>
#include <iomanip>

#include <bsoncxx/oid.hpp>
#include <fmt/format.h>
#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/grpcpp.h>

#include "server.pb.h"
#include "server.grpc.pb.h"

#include "spdlog/spdlog.h"

#include "util/helpers.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientWriter;

using server::Animation;
using server::AnimationMetadata;
using server::AnimationFilter;
using server::AnimationId;
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
using server::PlayAnimationRequest;
using server::PlayAnimationResponse;
using server::Playlist;
using server::PlaylistIdentifier;
using server::PlaylistFilter;
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


class CreatureServerClient {
public:
    explicit CreatureServerClient(const std::shared_ptr<Channel>& channel)
            : stub_(CreatureServer::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    Creature SearchCreatures(const std::string& name) {
        // Data we are sending to the server.
        CreatureName request;
        request.set_name(name);

        // Container for the data we expect from the server.
        Creature reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->SearchCreatures(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
            debug("✅ search ok!");
            return reply;
        } else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("search not found! 🚫");
            return reply;
        } else {
            critical("search not okay: {}: {}", to_string(status.error_code()), status.error_message());
            return reply;
        }
    }

    Creature GetCreature(const CreatureId& id) {

        debug("in GetCreature() with {}", creatures::creatureIdToString(id));

        // Container for the data we expect from the server.
        Creature reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->GetCreature(&context, id, &reply);

        // Act upon its status.
        if (status.ok()) {
            debug("✅ get ok!");
            return reply;
        } else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("get not found! 🚫");
            return reply;
        } else {
            critical("get not okay: {}: {}", static_cast<int32_t>(status.error_code()), status.error_message());
            return reply;
        }
    }

    server::DatabaseInfo CreateCreature(const Creature& creature) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->CreateCreature(&context, creature, &reply);

        if(status.ok()) {
            debug("got an OK from the server on save! ({})", reply.message());
        }
        else if(status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
            error("Creature {} already exists in the database", creature.name());
        }
        else {
            error("Unable to save a creature in the database: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

    server::DatabaseInfo UpdateCreature(const Creature& creature) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->UpdateCreature(&context, creature, &reply);

        if(status.ok()) {
            debug("Got an okay from the server on a creature update! ({})", reply.message());
        }
        else {
            error("Unable to update a creature in the database: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

    server::ListCreaturesResponse ListCreatures(const CreatureFilter& filter) {

        ClientContext context;
        server::ListCreaturesResponse reply;

        Status status = stub_->ListCreatures(&context, filter, &reply);

        if(status.ok()) {
            debug("got an OK from the server on a request to list all of the creatures");
        }
        else {
            error("An error happened while trying to list all of the creatures: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;
    }

    server::GetAllCreaturesResponse GetAllCreatures(const CreatureFilter& filter) {

        ClientContext context;
        server::GetAllCreaturesResponse reply;

        Status status = stub_->GetAllCreatures(&context, filter, &reply);

        if(status.ok()) {
            debug("got an OK from the server on a request to get all of the creatures");
        }
        else {
            error("An error happened while trying to list all of the creatures: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;
    }

    server::StreamFrameDataResponse StreamFrames() {
        StreamFrameDataResponse response;
        ClientContext context;
        // Create a writer for the stream of frames
        std::unique_ptr<ClientWriter<StreamFrameData>> writer(stub_->StreamFrames(&context, &response));

        // Some frames to the stream
        for (int i = 0; i <= 1000; i += 10) {

            StreamFrameData frame = StreamFrameData();

            // Send five test frames
            frame.set_creature_id("Beaky");
            frame.set_universe(3);

            char notdata[9] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9};

            std::vector<uint8_t> data;
            data.push_back(255);
            data.push_back(129);
            data.push_back(i);
            data.push_back(255);

            frame.set_data(data.data(), 4);
            writer->Write(frame);

            debug("sent");

            std::this_thread::sleep_for(std::chrono::milliseconds(20));

        }

        // Close the stream
        writer->WritesDone();

        // Receive the response and check the status
        Status status = writer->Finish();
        if (status.ok()) {

            info("server said: {}", status.error_message());
            response.set_message("Done!");
            return response;
        } else {

            auto errorMessage = fmt::format("Error processing frames: {}", status.error_message());
            error(errorMessage);

            response.set_message(errorMessage);
            return response;
        }
    }

    void StreamLogs(LogLevel level) {
        LogFilter filter;
        filter.set_level(level);

        ClientContext context;
        std::unique_ptr<grpc::ClientReader<LogItem>> reader(stub_->StreamLogs(&context, filter));

        LogItem log_item;
        while (reader->Read(&log_item)) {
            auto time_point = std::chrono::system_clock::from_time_t(log_item.timestamp().seconds());
            time_point += std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(log_item.timestamp().nanos()));
            std::time_t time_t_value = std::chrono::system_clock::to_time_t(time_point);
            std::tm local_tm = *std::localtime(&time_t_value);

            auto duration_since_epoch = time_point.time_since_epoch();
            auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration_since_epoch) % 1000;

            std::cout << "[" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << "." << std::setw(3) << std::setfill('0') << milliseconds.count() << "] "
                      << log_item.logger_name() << " [" << log_item.level() << "] "
                      << log_item.message() << std::endl;
        }

        Status status = reader->Finish();
        if (!status.ok()) {
            std::cerr << "StreamLogs RPC failed: " << status.error_message() << " (#" << status.error_code() << ")" << std::endl;
        }
    }

    server::DatabaseInfo CreateAnimation(const Animation& animation) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->CreateAnimation(&context, animation, &reply);

        if(status.ok()) {
            debug("got an OK from the server on save! ({})", reply.message());
        }
        else if(status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
            error("Animation {} already exists in the database", animation.metadata().title());
        }
        else {
            error("Unable to save an animation in the database: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

    server::DatabaseInfo UpdateAnimation(const Animation& animation) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->UpdateAnimation(&context, animation, &reply);

        if(status.ok()) {
            debug("Got an okay from the server on an animation update! ({})", reply.message());
        }
        else {
            error("Unable to update an animation in the database: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }



    server::ListAnimationsResponse ListAnimations(const AnimationFilter& filter) {

        ClientContext context;
        server::ListAnimationsResponse reply;

        Status status = stub_->ListAnimations(&context, filter, &reply);

        if(status.ok()) {
            debug("Got an OK from the server while trying to list all of the animations for creature {}", creatures::creatureIdToString(filter.creature_id()));
        }
        else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("No animations for creature {} found. (This might be expected!)", creatures::creatureIdToString(filter.creature_id()));
        }
        else {
            error("An error occurred while trying to get all of the animations for creature {}: {} ({})",
                  creatures::creatureIdToString(filter.creature_id()), status.error_message(), status.error_details());
        }

        return reply;
    }

    server::Animation GetAnimation(const AnimationId& id) {

        ClientContext context;
        Animation reply;

        Status status = stub_->GetAnimation(&context, id, &reply);

        if(status.ok()) {
            debug("Got an OK while loading one animation");
        }
        else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("Animation not found. (This might be expected!)");
        }
        else {
            error("An error happened while loading an animation. ID {}: {} ({})",
                  id._id(), status.error_message(), status.error_details());
        }

        return reply;
    }

    server::AnimationMetadata GetAnimationMetadata(const AnimationId& id) {

        ClientContext context;
        AnimationMetadata reply;

        Status status = stub_->GetAnimationMetadata(&context, id, &reply);

        if(status.ok()) {
            debug("Got an OK while loading one animation");
        }
        else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("Animation not found. (This might be expected!)");
        }
        else {
            error("An error happened while loading an animationIdentifier. ID {}: {} ({})",
                  creatures::animationIdToString(id), status.error_message(), status.error_details());
        }

        return reply;
    }

    server::PlayAnimationResponse PlayAnimation(const PlayAnimationRequest& request) {

        ClientContext context;
        PlayAnimationResponse response;

        Status status = stub_->PlayAnimation(&context, request, &response);

        if(status.ok()) {
            info("Played an animation!");
        }
        else {
            error("An error happened playing an animation! {} ({})",
                  status.error_message(), status.error_details());
        }

        return response;

    }

    server::PlaySoundResponse PlaySound(const server::PlaySoundRequest& request) {

        ClientContext context;
        server::PlaySoundResponse response;

        Status status = stub_->PlaySound(&context, request, &response);

        if(status.ok()) {
            info("Requested a sound to be played!");
        }
        else {
            error("An error happened playing a sound! {} ({})",
                  status.error_message(), status.error_details());
        }

        return response;

    }


    server::DatabaseInfo CreatePlaylist(const Playlist& playlist) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->CreatePlaylist(&context, playlist, &reply);

        if(status.ok()) {
            debug("got an OK from the server on playlist create! ({})", reply.message());
        }
        else if(status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
            error("Playlist {} already exists in the database", playlist.name());
        }
        else {
            error("Unable to save playlist in the database: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

    server::ListPlaylistsResponse ListPlaylists(const PlaylistFilter& filter) {

        ClientContext context;
        server::ListPlaylistsResponse reply;

        Status status = stub_->ListPlaylists(&context, filter, &reply);

        if(status.ok()) {
            debug("Got an OK from the server while trying to list all of the playlists");
        }
        else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("No playlists found. (This might be expected!)");
        }
        else {
            error("An error occurred while trying to get all of the playlists: {} ({})",
                 status.error_message(), status.error_details());
        }

        return reply;
    }

    server::Playlist GetPlaylist(const PlaylistIdentifier& id) {

        ClientContext context;
        Playlist reply;

        Status status = stub_->GetPlaylist(&context, id, &reply);

        if(status.ok()) {
            debug("Got an OK while loading one playlist");
        }
        else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("Playlist not found. (This might be expected!)");
        }
        else {
            error("An error happened while loading a playlist. ID {}: {} ({})",
                  id._id(), status.error_message(), status.error_details());
        }

        return reply;
    }


    server::DatabaseInfo UpdatePlaylist(const Playlist& playlist) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->UpdatePlaylist(&context, playlist, &reply);

        if(status.ok()) {
            debug("Got an okay from the server on a playlist update! ({})", reply.message());
        }
        else {
            error("Unable to update a playlist in the database: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

    server::PlaylistResponse StartPlaylist(const server::PlaylistRequest& request) {

        ClientContext context;
        server::PlaylistResponse reply;

        Status status = stub_->StartPlaylist(&context, request, &reply);

        if(status.ok()) {
            debug("Got an okay from the server on a request to play a playlist ({})", reply.message());
        }
        else {
            error("Unable to request to play a playlist: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }


    server::PlaylistResponse StopPlaylist(const server::PlaylistStopRequest& request) {

        ClientContext context;
        server::PlaylistResponse reply;

        Status status = stub_->StopPlaylist(&context, request, &reply);

        if(status.ok()) {
            debug("Got an okay from the server on a request to stop playback on a creature ({})", reply.message());
        }
        else {
            error("Unable to request to stop playback on a creature: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

    server::PlaylistStatus GetPlaylistStatus(const server::PlaylistRequest& request) {

        ClientContext context;
        server::PlaylistStatus reply;

        Status status = stub_->GetPlaylistStatus(&context, request, &reply);

        if(status.ok()) {
            debug("Got an okay from the server while getting a creature's playlist status");
        }
        else {
            error("Unable to get the playlist status of a creature: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;

    }

private:
    std::unique_ptr<CreatureServer::Stub> stub_;
};