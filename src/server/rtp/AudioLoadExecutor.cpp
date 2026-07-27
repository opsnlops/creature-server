#include "AudioLoadExecutor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace creatures::rtp {

AudioLoadExecutor::AudioLoadExecutor(std::size_t workerCount, std::size_t queueCapacity, StatsObserver statsObserver)
    : workerCount_(workerCount), queueCapacity_(queueCapacity), statsObserver_(std::move(statsObserver)) {
    if (workerCount_ == 0) {
        throw std::invalid_argument("AudioLoadExecutor requires at least one worker");
    }
    if (queueCapacity_ == 0) {
        throw std::invalid_argument("AudioLoadExecutor requires a non-zero queue capacity");
    }

    try {
        workers_.reserve(workerCount_);
        for (std::size_t index = 0; index < workerCount_; ++index) {
            workers_.emplace_back(&AudioLoadExecutor::workerLoop, this, index);
        }
    } catch (...) {
        {
            std::lock_guard lock(queueMutex_);
            shuttingDown_ = true;
        }
        workersStart_.count_down();
        queueCondition_.notify_all();
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
    // Do not let a worker touch executor state until every std::thread has
    // been published and construction is ready to return.
    workersStart_.count_down();
    publishStats();
}

AudioLoadExecutor::~AudioLoadExecutor() { shutdown(); }

AudioLoadExecutor::SubmitResult AudioLoadExecutor::submit(Job job) {
    SubmitResult result = SubmitResult::Accepted;
    {
        std::lock_guard lock(queueMutex_);
        if (shuttingDown_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            result = SubmitResult::ShuttingDown;
        } else {
            const auto discardedJobs = discardCancelledJobsLocked();
            cancelled_.fetch_add(discardedJobs, std::memory_order_relaxed);
            if (queue_.size() >= queueCapacity_) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                result = SubmitResult::QueueFull;
            } else {
                queue_.push_back(std::move(job));
                accepted_.fetch_add(1, std::memory_order_relaxed);
            }
            queued_.store(queue_.size(), std::memory_order_relaxed);
        }
    }

    publishStats();
    if (result == SubmitResult::Accepted) {
        queueCondition_.notify_one();
    }
    return result;
}

void AudioLoadExecutor::shutdown() {
    std::size_t discardedJobs = 0;
    {
        std::lock_guard lock(queueMutex_);
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        discardedJobs = queue_.size();
        queue_.clear();
        queued_.store(0, std::memory_order_relaxed);
        cancelled_.fetch_add(discardedJobs, std::memory_order_relaxed);
    }

    publishStats();
    queueCondition_.notify_all();
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    publishStats();
}

AudioLoadExecutor::Stats AudioLoadExecutor::getStats() const {
    return Stats{
        .workerCount = workerCount_,
        .queueCapacity = queueCapacity_,
        .active = active_.load(std::memory_order_relaxed),
        .queued = queued_.load(std::memory_order_relaxed),
        .accepted = accepted_.load(std::memory_order_relaxed),
        .completed = completed_.load(std::memory_order_relaxed),
        .rejected = rejected_.load(std::memory_order_relaxed),
        .cancelled = cancelled_.load(std::memory_order_relaxed),
        .failed = failed_.load(std::memory_order_relaxed),
    };
}

std::size_t AudioLoadExecutor::discardCancelledJobsLocked() {
    const auto initialSize = queue_.size();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [](const Job &job) {
                                    if (!job.isCancelled) {
                                        return false;
                                    }
                                    try {
                                        return job.isCancelled();
                                    } catch (...) {
                                        // The worker's top-level exception envelope will
                                        // route a broken predicate through its failure path.
                                        return false;
                                    }
                                }),
                 queue_.end());
    return initialSize - queue_.size();
}

void AudioLoadExecutor::workerLoop(std::size_t workerIndex) {
    workersStart_.wait();

    while (true) {
        Job job;
        {
            std::unique_lock lock(queueMutex_);
            try {
                queueCondition_.wait(lock, [this]() { return shuttingDown_ || !queue_.empty(); });
            } catch (const std::exception &ex) {
                spdlog::critical("RTP audio loader worker {} queue wait failed: {}", workerIndex, ex.what());
                return;
            } catch (...) {
                spdlog::critical("RTP audio loader worker {} queue wait failed with an unknown exception", workerIndex);
                return;
            }
            if (shuttingDown_) {
                return;
            }

            job = std::move(queue_.front());
            queue_.pop_front();
            queued_.store(queue_.size(), std::memory_order_relaxed);
            active_.fetch_add(1, std::memory_order_relaxed);
        }
        publishStats();

        bool cancelled = false;
        try {
            cancelled = job.isCancelled && job.isCancelled();
            if (!cancelled) {
                if (!job.run) {
                    throw std::invalid_argument("Audio load job has no run callback");
                }
                job.run();
                completed_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            recordFailure(job, std::current_exception());
        }

        if (cancelled) {
            cancelled_.fetch_add(1, std::memory_order_relaxed);
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
        publishStats();
    }
}

void AudioLoadExecutor::publishStats() const {
    if (!statsObserver_) {
        return;
    }
    std::lock_guard observerLock(statsObserverMutex_);
    try {
        // Serialize snapshot + publish so a slower callback cannot overwrite a
        // newer active/queued gauge value from another worker.
        statsObserver_(getStats());
    } catch (const std::exception &ex) {
        spdlog::error("RTP audio load stats observer failed: {}", ex.what());
    } catch (...) {
        spdlog::error("RTP audio load stats observer failed with an unknown exception");
    }
}

void AudioLoadExecutor::recordFailure(const Job &job, std::exception_ptr failure) {
    failed_.fetch_add(1, std::memory_order_relaxed);
    if (!job.onFailure) {
        spdlog::error("RTP audio load job '{}' failed without a failure handler", job.id);
        return;
    }

    try {
        job.onFailure(std::move(failure));
    } catch (const std::exception &ex) {
        spdlog::error("RTP audio load job '{}' failure handler threw: {}", job.id, ex.what());
    } catch (...) {
        spdlog::error("RTP audio load job '{}' failure handler threw an unknown exception", job.id);
    }
}

} // namespace creatures::rtp
