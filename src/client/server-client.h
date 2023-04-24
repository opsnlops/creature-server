
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

#include "messaging/server.pb.h"
#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"

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
            critical("search not okay: {}: {}", status.error_code(), status.error_message());
            return reply;
        }
    }

    Creature GetCreature(const CreatureId& id) {

        debug("in GetCreature() with {}");


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
            critical("get not okay: {}: {}", status.error_code(), status.error_message());
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

    server::FrameResponse StreamFrames() {
        FrameResponse response;
        ClientContext context;
        // Create a writer for the stream of frames
        std::unique_ptr<ClientWriter<Frame>> writer(stub_->StreamFrames(&context, &response));

        // Some frames to the stream
        for (int i = 0; i <= 1000; i += 10) {

            Frame frame = Frame();

            // Send five test frames
            frame.set_creature_name("Beaky");
            frame.set_dmx_offset(0);
            frame.set_universe(3);
            frame.set_sacn_ip("10.9.1.104");
            frame.set_number_of_motors(4);

            char notdata[9] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9};

            std::vector<uint8_t> data;
            data.push_back(255);
            data.push_back(129);
            data.push_back(i);
            data.push_back(255);

            frame.set_frame(data.data(), 4);
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


    server::ListAnimationsResponse ListAnimations(const AnimationFilter& filter) {

        ClientContext context;
        server::ListAnimationsResponse reply;

        Status status = stub_->ListAnimations(&context, filter, &reply);

        if(status.ok()) {
            debug("Got an OK from the server while trying to list all of the animations for creature type {}", filter.type());
        }
        else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("No animations for creature type {} found. (This might be expected!)", filter.type());
        }
        else {
            error("An error occured while trying to get all of the animations for creature type {}: {} ({})",
                  filter.type(), status.error_message(), status.error_details());
        }

        return reply;
    }

private:
    std::unique_ptr<CreatureServer::Stub> stub_;
};