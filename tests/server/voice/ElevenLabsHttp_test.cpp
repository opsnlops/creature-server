#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "server/voice/ElevenLabsHttp.h"

namespace creatures::voice::elevenlabs_http {

// Issue #193: every ElevenLabs call must borrow from ONE process-wide pool, or
// each streamed sentence pays a fresh TLS handshake.
TEST(ElevenLabsHttp, SharedConnectionPoolIsOneProcessWideHandle) {
    CURLSH *first = sharedConnectionPool();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(sharedConnectionPool(), first);

    // Threads see the same pool, and constructing calls concurrently (which
    // attaches the share under libcurl's lock callbacks) must be safe.
    std::vector<std::thread> threads;
    std::vector<CURLSH *> seen(8, nullptr);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        threads.emplace_back([&seen, i] {
            ElevenLabsCall call("not-a-real-key", "https://api.elevenlabs.io/v1/unused");
            EXPECT_TRUE(call.initOk());
            seen[i] = sharedConnectionPool();
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }
    for (const auto *pool : seen) {
        EXPECT_EQ(pool, first);
    }
}

TEST(ElevenLabsHttp, RecordTimingsToleratesNoSpanAndNoTransfer) {
    ElevenLabsCall call("not-a-real-key", "https://api.elevenlabs.io/v1/unused");
    ASSERT_TRUE(call.initOk());
    // Never performed, no span: must be a no-op rather than a crash.
    call.recordTimings(nullptr);
}

} // namespace creatures::voice::elevenlabs_http
