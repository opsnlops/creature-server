
#include "gtest/gtest.h"

#include "model/Creature.h"
#include "server/ws/HttpResponse.h"


namespace creatures ::ws {

    class HttpResponseTest : public ::testing::Test {
    protected:
        void SetUp() override {
            // Setup code before each test, if necessary
        }

        void TearDown() override {
            // Cleanup code after each test, if necessary
        }
    };

    TEST_F(HttpResponseTest, BasicResponseSerialization) {
        Creature creature{"id1", "BunnyBot", 100, 1, "Very hoppy"};
        HttpResponse response(HttpStatus::OK, creature);

        std::string expected_json = R"({"audio_channel":1,"channel_offset":100,"id":"id1","name":"BunnyBot","notes":"Very hoppy"})";
        EXPECT_EQ(response.getBody(), expected_json);
    }

    TEST_F(HttpResponseTest, HttpStatusCodeHandling) {
        HttpResponse response(HttpStatus::NotFound, nlohmann::json({{"error", "not found"}}));
        auto [code, message] = getHttpStatusMessage(response.getStatus());

        EXPECT_EQ(code, 404);
        EXPECT_EQ(message, "Not Found");
        EXPECT_EQ(response.getBody(), R"({"error":"not found"})");
    }

    TEST_F(HttpResponseTest, UnicodeAndEmojiHandling) {
        Creature creature{"id2", "Mango", 101, 2, "Loves 🥭"};
        HttpResponse response(HttpStatus::OK, creature);

        std::string expected_json = R"({"audio_channel":2,"channel_offset":101,"id":"id2","name":"Mango","notes":"Loves 🥭"})";
        EXPECT_EQ(response.getBody(), expected_json);
    }

    TEST_F(HttpResponseTest, ErrorConditionHandling) {

        EXPECT_THROW({
                         HttpResponse response(HttpStatus::InternalServerError,
                                               nlohmann::json::parse("{invalid_json}"));
                     }, std::exception);
    }

}  // namespace creatures
