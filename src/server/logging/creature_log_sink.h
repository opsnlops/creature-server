
#pragma once

#include <chrono>

#include "spdlog/spdlog.h"
#include "spdlog/common.h"
#include "spdlog/details/log_msg.h"
#include "spdlog/sinks/base_sink.h"
#include <spdlog/details/synchronous_factory.h>

#include "model/LogItem.h"


#include "concurrentqueue.h"


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
        explicit CreatureLogSink(moodycamel::ConcurrentQueue<creatures::LogItem> &queue) : queue_(queue) {}

    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override {

            auto logItem = creatures::LogItem();

//            *logItem.mutable_timestamp() = to_protobuf_timestamp(msg.time);
//            logItem.set_message(std::string(msg.payload.data(), msg.payload.size()));
//            logItem.set_logger_name(std::string(msg.logger_name.data(), msg.logger_name.size()));
//            logItem.set_thread_id(msg.thread_id);
//
//            // Convert the log level to our own
//            switch (msg.level) {
//                case level::trace:
//                    logItem.set_level(LogLevel::trace);
//                    break;
//                case level::debug:
//                    logItem.set_level(LogLevel::debug);
//                    break;
//                case level::info:
//                    logItem.set_level(LogLevel::info);
//                    break;
//                case level::warn:
//                    logItem.set_level(LogLevel::warn);
//                    break;
//                case level::err:
//                    logItem.set_level(LogLevel::error);
//                    break;
//                case level::critical:
//                    logItem.set_level(LogLevel::critical);
//                    break;
//                case level::off:
//                    logItem.set_level(LogLevel::off);
//                    break;
//                default:
//                    logItem.set_level(LogLevel::unknown);
//            }

            // Off to the queue with you!
            queue_.enqueue(logItem);
        }

        void flush_() override {}

    private:
        moodycamel::ConcurrentQueue<creatures::LogItem> &queue_;




    };
}