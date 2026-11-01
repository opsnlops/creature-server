#include <gtest/gtest.h>

#include "server/config/MongoUri.h"

namespace creatures::mongo {
namespace {

TEST(MongoUri, AddsEveryFiniteDeadline) {
    EXPECT_EQ(normalizeUri("mongodb://db.example/creature_server"),
              "mongodb://db.example/creature_server?serverSelectionTimeoutMS=750&connectTimeoutMS=500&"
              "socketTimeoutMS=1000&waitQueueTimeoutMS=250&wTimeoutMS=750");
}

TEST(MongoUri, PreservesLowerValuesAndClampsHigherValues) {
    EXPECT_EQ(normalizeUri("mongodb://db/?serverSelectionTimeoutMS=50&connectTimeoutMS=5000&retryWrites=true"),
              "mongodb://db/?retryWrites=true&serverSelectionTimeoutMS=50&connectTimeoutMS=500&socketTimeoutMS=1000&"
              "waitQueueTimeoutMS=250&wTimeoutMS=750");
}

TEST(MongoUri, InvalidZeroAndDuplicateValuesCannotDisableBounds) {
    EXPECT_EQ(normalizeUri("mongodb://db/?socketTimeoutMS=0&waitQueueTimeoutMS=nope&SOCKETTIMEOUTMS=25&"
                           "serverSelectionTimeoutMS=100&serverSelectionTimeoutMS=20"),
              "mongodb://db/?serverSelectionTimeoutMS=20&connectTimeoutMS=500&socketTimeoutMS=25&"
              "waitQueueTimeoutMS=250&wTimeoutMS=750");
}

TEST(MongoUri, NormalizationIsIdempotent) {
    const auto once = normalizeUri("mongodb+srv://db.example/?retryReads=true");
    EXPECT_EQ(normalizeUri(once), once);
}

TEST(MongoUri, RedactsCredentials) {
    EXPECT_EQ(redactUri("mongodb://user:secret@db.example/?retryWrites=true"),
              "mongodb://<credentials>@db.example/?retryWrites=true");
    EXPECT_EQ(redactUri("mongodb://db.example/?retryWrites=true"), "mongodb://db.example/?retryWrites=true");
}

} // namespace
} // namespace creatures::mongo
