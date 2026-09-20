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
    EXPECT_EQ(document.at("paths").size(), 106);

    std::size_t operationCount = 0;
    for (const auto &[path, operations] : document.at("paths").items()) {
        static_cast<void>(path);
        operationCount += operations.size();
    }
    EXPECT_EQ(operationCount, 125);

    // Every operation carries its own documentation from the manifest, not
    // just the handler name, and the readable tag groups survived the port.
    for (const auto &[path, operations] : document.at("paths").items()) {
        for (const auto &[method, operation] : operations.items()) {
            const auto summary = operation.at("summary").get<std::string>();
            EXPECT_FALSE(summary.empty()) << method << " " << path;
            EXPECT_NE(summary, operation.at("operationId").get<std::string>()) << method << " " << path;
            EXPECT_FALSE(operation.at("tags").empty()) << method << " " << path;
            EXPECT_TRUE(operation.at("responses").contains("200") || operation.at("responses").contains("201") ||
                        operation.at("responses").contains("202") || operation.at("responses").contains("101"))
                << method << " " << path;
        }
    }
    const auto &preview =
        document.at("paths").at("/api/v1/animation/dialog/preview/share/{cache_key}/{filename}").at("get");
    EXPECT_EQ(preview.at("tags").at(0), "Multi-character Dialog");
    EXPECT_TRUE(preview.at("responses").at("200").at("content").contains("audio/mpeg"));
    EXPECT_TRUE(preview.at("responses").at("200").at("content").contains("audio/ogg"));
    const auto &prune = document.at("paths").at("/api/v1/debug/cache/audio/prune").at("post");
    bool sawDryRun = false;
    for (const auto &parameter : prune.at("parameters")) {
        sawDryRun = sawDryRun || (parameter.at("in") == "query" && parameter.at("name") == "dry_run");
    }
    EXPECT_TRUE(sawDryRun);
    const auto &stt = document.at("paths").at("/api/v1/stt/transcribe").at("post");
    EXPECT_TRUE(stt.at("requestBody").at("required").get<bool>());
    EXPECT_TRUE(stt.at("requestBody").at("content").contains("application/octet-stream"));
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
