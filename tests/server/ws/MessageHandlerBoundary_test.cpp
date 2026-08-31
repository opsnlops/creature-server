#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>

#include "server/ws/messaging/IMessageHandler.h"

namespace creatures::ws {
namespace {

class TestMessageHandler final : public IMessageHandler {
  public:
    using IMessageHandler::IMessageHandler;

    bool processMessage(const nlohmann::json &, std::string_view, std::string_view,
                        std::shared_ptr<SamplingSpan>) override {
        return true;
    }
};

TEST(MessageHandlerBoundary, AcceptsAnExplicitFrameworkNeutralLogger) {
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("message-handler-boundary", sink);

    EXPECT_NO_THROW({ TestMessageHandler handler(logger); });
}

TEST(MessageHandlerBoundary, RejectsAMissingLoggerAtConstruction) {
    EXPECT_THROW(TestMessageHandler(nullptr), std::invalid_argument);
}

} // namespace
} // namespace creatures::ws
