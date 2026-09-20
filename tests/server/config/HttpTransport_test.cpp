#include <gtest/gtest.h>

#include "server/config.h"
#include "server/config/Configuration.h"

namespace creatures {
namespace {

TEST(HttpTransport, UWebSocketsIsTheRuntimeDefault) {
    const Configuration configuration;
    EXPECT_EQ(configuration.getHttpTransport(), Configuration::HttpTransport::UWebSockets);
    EXPECT_STREQ(DEFAULT_HTTP_TRANSPORT, "uwebsockets");
}

TEST(HttpTransport, OatppRemainsACompiledRollbackChoice) {
    EXPECT_NE(Configuration::HttpTransport::Oatpp, Configuration::HttpTransport::UWebSockets);
}

} // namespace
} // namespace creatures
