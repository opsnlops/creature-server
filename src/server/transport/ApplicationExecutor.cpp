#include "server/transport/ApplicationExecutor.h"

#include <utility>

#include "util/threadName.h"

namespace creatures::transport {

ApplicationExecutor::ApplicationExecutor(const std::size_t workerCount, const std::size_t queueLimit)
    : queueLimit_(queueLimit) {
    workers_.reserve(workerCount);
    for (std::size_t index = 0; index < workerCount; ++index) {
        workers_.emplace_back([this](const std::stop_token stopToken) { run(stopToken); });
    }
}

ApplicationExecutor::~ApplicationExecutor() {
    requestStop();
    join();
}

bool ApplicationExecutor::trySubmit(Task task, Abandoned abandoned) {
    std::lock_guard lock(mutex_);
    if (stopping_ || queue_.size() >= queueLimit_) {
        return false;
    }
    queue_.push_back({.run = std::move(task), .abandoned = std::move(abandoned)});
    condition_.notify_one();
    return true;
}

void ApplicationExecutor::requestStop() {
    std::deque<QueuedTask> abandoned;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        abandoned.swap(queue_);
    }
    for (auto &worker : workers_) {
        worker.request_stop();
    }
    condition_.notify_all();
    for (auto &task : abandoned) {
        if (task.abandoned) {
            task.abandoned();
        }
    }
}

void ApplicationExecutor::join() {
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t ApplicationExecutor::queued() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

bool ApplicationExecutor::stopping() const {
    std::lock_guard lock(mutex_);
    return stopping_;
}

void ApplicationExecutor::run(const std::stop_token stopToken) {
    setThreadName("http-app-worker");
    while (!stopToken.stop_requested()) {
        QueuedTask task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock,
                            [this, &stopToken] { return stopping_ || stopToken.stop_requested() || !queue_.empty(); });
            if (stopping_ || stopToken.stop_requested()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task.run(stopToken);
        } catch (...) {
            // Request tasks translate errors. No exception may terminate a worker.
        }
    }
}

} // namespace creatures::transport
