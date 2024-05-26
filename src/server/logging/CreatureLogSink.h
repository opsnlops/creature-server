
#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

#include "blockingconcurrentqueue.h"

#include "spdlog/spdlog.h"
#include "spdlog/common.h"
#include "spdlog/details/log_msg.h"
#include "spdlog/sinks/base_sink.h"
#include <spdlog/details/synchronous_factory.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/Types.hpp>

#include "model/LogItem.h"
#include "server/ws/dto/websocket/LogMessage.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "util/MessageQueue.h"


namespace spdlog::sinks {

    /**
     * A custom log sink that drops messages into a concurrent queue
     *
     * This is used to allow clients to see the log messages over the wire if they
     * would like to, using the StreamLogs rpc call
     *
     * @tparam Mutex From the base_sink
     */
    template<typename Mutex>
    class CreatureLogSink : public base_sink<Mutex> {
    public:
        explicit CreatureLogSink(std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> &queue) : queue_(queue) {
            jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        }

    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override {

            /*
             * WARNING:
             *
             * It might not be obvious, but any use of the logger in here results in a
             * recursive call to sink_it_. 😅
             */

            auto logItem = convertSpdlogToLogItem(msg);
            auto logItemDto = creatures::convertToDto(logItem);


            auto message = oatpp::Object<creatures::ws::LogMessage>::createShared();
            message->command = toString(creatures::ws::MessageType::LogMessage);
            message->payload = logItemDto;

            std::string messageAsString = jsonMapper->writeToString(message);

            // Off to the queue with you!
            queue_->enqueue(std::move(messageAsString));

        }

        void flush_() override {}

        static std::string formatSpdlogTimestamp(const spdlog::details::log_msg& log_msg) {
            // Extract the time_point directly from the log_msg
            auto time_point = log_msg.time;

            // Convert to time_t for easy formatting
            auto time_t_time = std::chrono::system_clock::to_time_t(time_point);

            // Format the time using std::put_time
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time_t_time), "%Y-%m-%dT%H:%M:%SZ");
            return ss.str();
        }

        static creatures::LogLevel mapSpdlogLevel(spdlog::level::level_enum spdlog_level) {
            switch (spdlog_level) {
                case spdlog::level::trace: return creatures::LogLevel::trace;
                case spdlog::level::debug: return creatures::LogLevel::debug;
                case spdlog::level::info: return creatures::LogLevel::info;
                case spdlog::level::warn: return creatures::LogLevel::warn;
                case spdlog::level::err: return creatures::LogLevel::error;
                case spdlog::level::critical: return creatures::LogLevel::critical;
                case spdlog::level::off: return creatures::LogLevel::off;
                default: return creatures::LogLevel::unknown;  // Default to unknown
            }
        }

        static creatures::LogItem convertSpdlogToLogItem(const spdlog::details::log_msg& log_msg) {

            auto logItem = creatures::LogItem();

            logItem.message = std::string(log_msg.payload.data(), log_msg.payload.size());
            logItem.logger_name = std::string(log_msg.logger_name.data(), log_msg.logger_name.size());
            logItem.thread_id = log_msg.thread_id;
            logItem.timestamp = formatSpdlogTimestamp(log_msg);
            logItem.level = mapSpdlogLevel(log_msg.level);

            return logItem;
        }

    private:
        std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> &queue_;
        std::shared_ptr<oatpp::parser::json::mapping::ObjectMapper> jsonMapper;

    };
}