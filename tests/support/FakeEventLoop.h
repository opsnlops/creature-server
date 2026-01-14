#pragma once

#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "server/namespace-stuffs.h"

namespace creatures::testing {

struct ScheduledCall {
    framenum_t frame;
    std::function<void()> fn;
};

class FakeEventLoop {
  public:
    void schedule(framenum_t frame, std::function<void()> fn);
    void runUntilEmpty();

    [[nodiscard]] bool empty() const { return queue_.empty(); }
    [[nodiscard]] framenum_t currentFrame() const { return currentFrame_; }
    [[nodiscard]] const std::vector<framenum_t> &executedFrames() const { return executedFrames_; }

  private:
    struct Compare {
        bool operator()(const ScheduledCall &lhs, const ScheduledCall &rhs) const {
            return lhs.frame > rhs.frame;
        }
    };

    std::priority_queue<ScheduledCall, std::vector<ScheduledCall>, Compare> queue_;
    framenum_t currentFrame_{0};
    std::vector<framenum_t> executedFrames_;
};

} // namespace creatures::testing
