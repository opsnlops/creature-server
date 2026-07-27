#pragma once

#include <cstdint>
#include <string>

namespace creatures::rtp {

/**
 * Business and trace context carried across the event-loop/output-worker
 * boundary. Successful packet sends remain untraced; this context makes the
 * failure-only output span queryable back to the request or animation that
 * produced it.
 */
struct AsyncAudioTraceContext {
    std::string triggerTraceId;
    std::string triggerSpanId;
    std::string sessionId;
    std::string animationId;
    std::string soundFile;
    uint64_t enqueueFrame{0};
};

} // namespace creatures::rtp
