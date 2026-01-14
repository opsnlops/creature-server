#include "support/FakeEventLoop.h"

namespace creatures::testing {

void FakeEventLoop::schedule(framenum_t frame, std::function<void()> fn) {
    queue_.push(ScheduledCall{frame, std::move(fn)});
}

void FakeEventLoop::runUntilEmpty() {
    while (!queue_.empty()) {
        auto next = queue_.top();
        queue_.pop();
        currentFrame_ = next.frame;
        executedFrames_.push_back(next.frame);
        if (next.fn) {
            next.fn();
        }
    }
}

} // namespace creatures::testing
