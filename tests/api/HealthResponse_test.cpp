#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/JsonResponse.h"

namespace creatures::api {
namespace {

TEST(HealthResponse, CarriesTheVersionAlongsideTheStatusEnvelope) {
    const auto json = healthResponseToJson(makeHealthResponse(200, "Server is operational", "3.51.8"));

    EXPECT_EQ(json.at("status"), STATUS_OK);
    EXPECT_EQ(json.at("code"), 200);
    EXPECT_EQ(json.at("message"), "Server is operational");
    EXPECT_EQ(json.at("version"), "3.51.8");
    EXPECT_EQ(json.size(), 4u) << "health answers with exactly these four keys";
}

// The health envelope has to stay byte-identical to a status response apart
// from the added version, because anything already parsing health as a status
// envelope must keep working.
TEST(HealthResponse, EnvelopeMatchesAStatusResponseApartFromTheVersion) {
    const auto health = healthResponseToJson(makeHealthResponse(200, "Server is operational", "3.51.8"));
    const auto status = statusResponseToJson(makeStatusResponse(200, "Server is operational"));

    auto withoutVersion = health;
    withoutVersion.erase("version");
    EXPECT_EQ(withoutVersion, status);
}

// The version is never asserted to be a real build here; it is whatever the
// handler was given. What matters is that it is passed through untouched, so a
// reader can trust it to be the running build rather than a sanitised guess.
TEST(HealthResponse, PassesTheVersionThroughUntouched) {
    EXPECT_EQ(healthResponseToJson(makeHealthResponse(200, "ok", "10.0.123")).at("version"), "10.0.123");
    EXPECT_EQ(healthResponseToJson(makeHealthResponse(200, "ok", "")).at("version"), "");
}

} // namespace
} // namespace creatures::api
