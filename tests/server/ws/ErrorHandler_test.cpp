#include <gtest/gtest.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "server/ws/ErrorHandler.h"

// ===========================================================================
// Error mapping for malformed request bodies (issue #129)
//
// oatpp names the internal function that raised a JSON failure, and which one
// it was is exactly what tells us whose fault it is:
//
//   Deserializer::* / Utils::*  -> reading the REQUEST  -> 400
//   Serializer::*               -> writing OUR response -> 500
//
// Before this, only a few Deserializer spellings were recognised, so a body
// that failed at the earlier preparse step came back 500. That tells a caller
// to retry or page someone when they should be fixing their request.
// ===========================================================================

namespace {

using Status = oatpp::web::protocol::http::Status;

std::shared_ptr<creatures::ws::ErrorHandler> makeHandler() {
    return std::make_shared<creatures::ws::ErrorHandler>(oatpp::parser::json::mapping::ObjectMapper::createShared());
}

/// Run a 500-with-message through the handler and report what came out.
struct Handled {
    int code;
    std::string message;
};

Handled handle(const std::string &message, int inboundCode = 500) {
    auto handler = makeHandler();
    const Status inbound = inboundCode == 500 ? Status::CODE_500 : Status::CODE_400;
    auto response = handler->handleError(inbound, oatpp::String(message.c_str()), {});
    return {response->getStatus().code, message};
}

} // namespace

TEST(ErrorHandlerJsonFaultTest, PreparseFailureBecomes400) {
    // The exact message from #129 — an unterminated string in the body.
    EXPECT_EQ(handle("[oatpp::parser::json::Utils::preparseString()]: Error. '\"' - expected").code, 400);
}

TEST(ErrorHandlerJsonFaultTest, EveryJsonInputPathBecomes400) {
    for (const auto *m : {"[oatpp::parser::json::Utils::preparseString()]: Error. '\"' - expected",
                          "[oatpp::parser::json::Utils::parseString()]: Error. '\"' - expected",
                          "[oatpp::parser::json::Utils::parseStringToStdString()]: Error.",
                          "[oatpp::parser::json::mapping::Deserializer::readObject()]: Error. Unknown field 'nope'",
                          "[oatpp::parser::json::mapping::Deserializer::deserializeCollection()]: Error.",
                          "[oatpp::parser::json::mapping::Deserializer::guessType()]: Error."}) {
        EXPECT_EQ(handle(m).code, 400) << m;
    }
}

TEST(ErrorHandlerJsonFaultTest, OurOwnSerializationFailureStays500) {
    // Serializer errors mean WE produced something unencodable. That is a
    // genuine server fault and must not be blamed on the caller.
    for (const auto *m : {"[oatpp::parser::json::mapping::Serializer::serialize()]: Error. Unknown type",
                          "[oatpp::parser::json::mapping::Serializer::serializeMap()]: Error."}) {
        EXPECT_EQ(handle(m).code, 500) << m;
    }
}

TEST(ErrorHandlerJsonFaultTest, UnrelatedServerErrorsStay500) {
    for (const auto *m : {"Database connection lost", "std::bad_alloc", ""}) {
        EXPECT_EQ(handle(m).code, 500) << m;
    }
}

TEST(ErrorHandlerJsonFaultTest, DoesNotTouchStatusesThatAreAlreadyCorrect) {
    // A 400 that arrives as a 400 passes straight through — the remap only
    // ever rescues a misreported 500.
    EXPECT_EQ(handle("[oatpp::parser::json::Utils::preparseString()]: Error.", 400).code, 400);
}
