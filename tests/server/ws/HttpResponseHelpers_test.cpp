#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "api/JsonResponse.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/StatusDto.h"
#include "util/Result.h"

namespace creatures::ws {

TEST(HttpResponseHelpers, DefaultStatusForCodePicksCorrectVocabulary) {
    EXPECT_STREQ(defaultStatusForCode(200), STATUS_OK);
    EXPECT_STREQ(defaultStatusForCode(201), STATUS_OK);
    EXPECT_STREQ(defaultStatusForCode(299), STATUS_OK);

    EXPECT_STREQ(defaultStatusForCode(404), STATUS_NOT_FOUND);

    EXPECT_STREQ(defaultStatusForCode(400), STATUS_ERROR);
    EXPECT_STREQ(defaultStatusForCode(403), STATUS_ERROR);
    EXPECT_STREQ(defaultStatusForCode(409), STATUS_ERROR);
    EXPECT_STREQ(defaultStatusForCode(500), STATUS_ERROR);
    EXPECT_STREQ(defaultStatusForCode(503), STATUS_ERROR);
}

TEST(HttpResponseHelpers, BuildStatusDtoForCommonCodes) {
    auto bad = buildStatusDto(400, "scriptId must be a UUID");
    EXPECT_EQ(std::string(*bad->status), "error");
    EXPECT_EQ(*bad->code, 400u);
    EXPECT_EQ(std::string(*bad->message), "scriptId must be a UUID");

    auto missing = buildStatusDto(404, "no such creature");
    EXPECT_EQ(std::string(*missing->status), "not_found");
    EXPECT_EQ(*missing->code, 404u);
    EXPECT_EQ(std::string(*missing->message), "no such creature");

    auto boom = buildStatusDto(500, "database unavailable");
    EXPECT_EQ(std::string(*boom->status), "error");
    EXPECT_EQ(*boom->code, 500u);
    EXPECT_EQ(std::string(*boom->message), "database unavailable");

    auto created = buildStatusDto(201, "DialogScript created");
    EXPECT_EQ(std::string(*created->status), "ok");
    EXPECT_EQ(*created->code, 201u);
}

TEST(HttpResponseHelpers, BuildStatusDtoHonorsOverride) {
    // Most call sites won't use this, but a 200 response that wants to
    // surface a soft "not_found" hint (e.g. /validate flagging missing
    // creature ids) can override the default.
    auto soft = buildStatusDto(200, "validation surfaced missing ids", STATUS_NOT_FOUND);
    EXPECT_EQ(std::string(*soft->status), "not_found");
    EXPECT_EQ(*soft->code, 200u);
}

TEST(JsonResponse, StatusResponseHasExactCanonicalShape) {
    const auto json = api::statusResponseToJson(api::makeStatusResponse(404, "no such creature"));

    EXPECT_EQ(json, nlohmann::json({{"status", "not_found"}, {"code", 404}, {"message", "no such creature"}}));
    EXPECT_FALSE(json.contains("session_id"));
}

TEST(JsonResponse, StatusResponseIncludesPresentSessionId) {
    const auto json = api::statusResponseToJson(
        api::makeStatusResponse(200, "scheduled", api::STATUS_OK, "c9628a84-dcad-4ba9-a2d7-35b495fc08c3"));

    EXPECT_EQ(json, nlohmann::json({{"status", "ok"},
                                    {"code", 200},
                                    {"message", "scheduled"},
                                    {"session_id", "c9628a84-dcad-4ba9-a2d7-35b495fc08c3"}}));
}

TEST(JsonResponse, InvalidUtf8IsReplacedInsteadOfThrowing) {
    const std::string invalidMessage = std::string("bad byte: ") + static_cast<char>(0xff);
    const auto serialization =
        api::serializeJson(api::statusResponseToJson(api::makeStatusResponse(500, invalidMessage, api::STATUS_ERROR)));
    const auto parsed = nlohmann::json::parse(serialization.bytes);

    EXPECT_EQ(parsed["message"], "bad byte: �");
    EXPECT_TRUE(serialization.invalidUtf8Replaced);
}

TEST(JsonResponse, ValidUtf8NeedsNoRepair) {
    const auto serialization =
        api::serializeJson(api::statusResponseToJson(api::makeStatusResponse(200, "Good creature 🥭", api::STATUS_OK)));

    EXPECT_FALSE(serialization.invalidUtf8Replaced);
    EXPECT_EQ(nlohmann::json::parse(serialization.bytes)["message"], "Good creature 🥭");
}

TEST(JsonResponse, ListResponseSerializesEmptyAndPopulatedRanges) {
    const std::vector<int> empty;
    EXPECT_EQ(api::listResponseToJson(empty, [](int item) { return nlohmann::json(item); }),
              nlohmann::json({{"count", 0}, {"items", nlohmann::json::array()}}));

    const std::vector<int> items{2, 4, 8};
    EXPECT_EQ(api::listResponseToJson(items, [](int item) { return nlohmann::json{{"value", item}}; }),
              nlohmann::json({{"count", 3}, {"items", {{{"value", 2}}, {{"value", 4}}, {{"value", 8}}}}}));
}

TEST(HttpResponseHelpers, ServerErrorToHttpStatusCodeIsExhaustive) {
    EXPECT_EQ(creatures::serverErrorToStatusCode(creatures::ServerError::NotFound), 404);
    EXPECT_EQ(creatures::serverErrorToStatusCode(creatures::ServerError::Forbidden), 403);
    EXPECT_EQ(creatures::serverErrorToStatusCode(creatures::ServerError::InvalidData), 400);
    EXPECT_EQ(creatures::serverErrorToStatusCode(creatures::ServerError::Conflict), 409);
    EXPECT_EQ(creatures::serverErrorToStatusCode(creatures::ServerError::InternalError), 500);
    EXPECT_EQ(creatures::serverErrorToStatusCode(creatures::ServerError::DatabaseError), 500);
}

TEST(HttpResponseHelpers, StatusVocabularyConstantsAreLowercase) {
    // Pinned so a well-meaning future cleanup doesn't accidentally re-introduce
    // the "ERROR" / "OK" caps mix that motivated issue #16.
    EXPECT_EQ(std::string(STATUS_OK), "ok");
    EXPECT_EQ(std::string(STATUS_ERROR), "error");
    EXPECT_EQ(std::string(STATUS_NOT_FOUND), "not_found");
}

} // namespace creatures::ws
