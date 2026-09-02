#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "server/transport/ApiDocumentation.h"

namespace creatures::transport {
namespace {

TEST(ApiDocumentation, ProducesAParseableOpenApiCatalog) {
    const auto document = nlohmann::json::parse(openApiDocument());
    EXPECT_EQ(document.at("openapi"), "3.1.0");
    EXPECT_TRUE(document.at("paths").contains("/api/v1/health"));
    EXPECT_TRUE(document.at("paths").contains("/api/docs"));
    EXPECT_TRUE(document.at("paths").contains("/api/openapi.json"));
    EXPECT_EQ(document.at("paths").size(), 92);

    std::size_t operationCount = 0;
    for (const auto &[path, operations] : document.at("paths").items()) {
        static_cast<void>(path);
        operationCount += operations.size();
    }
    EXPECT_EQ(operationCount, 109);
}

TEST(ApiDocumentation, BrowserIsSelfContainedAndUsesTheLocalCatalog) {
    const auto html = apiBrowserHtml();
    EXPECT_NE(html.find("/api/openapi.json"), std::string_view::npos);
    EXPECT_NE(html.find("Filter routes"), std::string_view::npos);
    EXPECT_EQ(html.find("https://"), std::string_view::npos);
    EXPECT_EQ(html.find("http://"), std::string_view::npos);
}

} // namespace
} // namespace creatures::transport
