#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace creatures::transport {

/** Fixed-size, bounded executor for synchronous request application work. */
class ApplicationExecutor {
  public:
    using Task = std::function<void(std::stop_token)>;
    using Abandoned = std::function<void()>;

    ApplicationExecutor(std::size_t workerCount, std::size_t queueLimit);
    ~ApplicationExecutor();

    ApplicationExecutor(const ApplicationExecutor &) = delete;
    ApplicationExecutor &operator=(const ApplicationExecutor &) = delete;

    bool trySubmit(Task task, Abandoned abandoned = {});
    void requestStop();
    void join();

    [[nodiscard]] std::size_t queued() const;
    [[nodiscard]] bool stopping() const;

  private:
    struct QueuedTask {
        Task run;
        Abandoned abandoned;
    };

    void run(std::stop_token stopToken);

    const std::size_t queueLimit_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedTask> queue_;
    std::vector<std::jthread> workers_;
    bool stopping_{false};
};

} // namespace creatures::transport
