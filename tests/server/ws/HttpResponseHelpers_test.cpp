#include <string>

#include <gtest/gtest.h>

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
