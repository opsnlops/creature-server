
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/LogItem.h"

namespace creatures {

    class LogItemTest : public ::testing::Test {
    protected:
        LogItem logItem; // Test fixture

        void SetUp() override {
            // Set up your test fixture with known values
            logItem.level = LogLevel::error;
            logItem.message = "An error occurred";
            logItem.logger_name = "SystemLogger";
            logItem.thread_id = 123456;
        }
    };

    // Test serialization to JSON
    TEST_F(LogItemTest, SerializesToJsonCorrectly) {
        nlohmann::json j = logItem;

        // Assert that all fields are serialized correctly
        EXPECT_EQ(j["level"], "error");
        EXPECT_EQ(j["message"], logItem.message);
        EXPECT_EQ(j["logger_name"], logItem.logger_name);
        EXPECT_EQ(j["thread_id"], logItem.thread_id);
    }

    // Test deserialization from JSON
    TEST_F(LogItemTest, DeserializesFromJsonCorrectly) {
        nlohmann::json j = {
                {"level", "error"},
                {"message", "An error occurred"},
                {"logger_name", "SystemLogger"},
                {"thread_id", 123456}
        };

        auto new_logItem = j.get<LogItem>();

        // Assert that all fields are deserialized correctly
        EXPECT_EQ(new_logItem.level, LogLevel::error);
        EXPECT_EQ(new_logItem.message, logItem.message);
        EXPECT_EQ(new_logItem.logger_name, logItem.logger_name);
        EXPECT_EQ(new_logItem.thread_id, logItem.thread_id);
    }

} // namespace creatures
