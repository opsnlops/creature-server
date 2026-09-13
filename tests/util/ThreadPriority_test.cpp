#include <gtest/gtest.h>

#include <thread>

#include "util/ThreadPriority.h"

// Issue #197: dropping to background priority is best-effort and must never
// throw or affect the calling thread's ability to continue.
TEST(ThreadPriority, LoweringIsBestEffortAndSafe) {
    bool lowered = false;
    std::thread worker([&] { lowered = creatures::util::lowerCurrentThreadPriority(); });
    worker.join();
    SUCCEED() << "lowered=" << lowered;
}
