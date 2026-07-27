#include "AudioLoadExecutor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace creatures::rtp {

namespace {

constexpr std::size_t MAX_CANCELLATION_CHECKS_PER_ADMISSION = 64;

} // namespace

AudioLoadExecutor::Reservation::~Reservation() {
    if (owner_) {
        owner_->releaseReservation();
    }
}

AudioLoadExecutor::Reservation::Reservation(Reservation &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

AudioLoadExecutor::Reservation &AudioLoadExecutor::Reservation::operator=(Reservation &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (owner_) {
        owner_->releaseReservation();
    }
    owner_ = std::exchange(other.owner_, nullptr);
    return *this;
}

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
            accepting_ = false;
            shuttingDown_ = true;
        }
        stopRequested_.store(true, std::memory_order_release);
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
    auto reserveResult = tryReserve();
    if (reserveResult.result != SubmitResult::Accepted) {
        return reserveResult.result;
    }
    return submit(std::move(reserveResult.reservation), std::move(job));
}

AudioLoadExecutor::ReserveResult AudioLoadExecutor::tryReserve() {
    SubmitResult result = SubmitResult::Accepted;
    std::vector<Job> discardedJobs;
    {
        std::lock_guard lock(queueMutex_);
        if (stopRequested_.load(std::memory_order_acquire) || !accepting_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            result = SubmitResult::ShuttingDown;
        } else {
            discardedJobs = takeCancelledJobsLocked();
            cancelled_.fetch_add(discardedJobs.size(), std::memory_order_relaxed);
            inFlight_ -= discardedJobs.size();
            enqueueCancellationNotificationsLocked(discardedJobs, CancellationReason::SessionCancelled);
            if (inFlight_ >= workerCount_ + queueCapacity_) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                result = SubmitResult::QueueFull;
            } else {
                ++inFlight_;
                ++reservations_;
                reserved_.store(reservations_, std::memory_order_relaxed);
            }
            queued_.store(queue_.size(), std::memory_order_relaxed);
        }
    }

    publishStats();
    if (!discardedJobs.empty()) {
        queueCondition_.notify_one();
    }
    if (result != SubmitResult::Accepted) {
        return ReserveResult{.result = result};
    }
    return ReserveResult{.result = result, .reservation = Reservation(this)};
}

AudioLoadExecutor::SubmitResult AudioLoadExecutor::submit(Reservation reservation, Job job) {
    if (reservation.owner_ != this) {
        throw std::invalid_argument("Audio load reservation belongs to a different executor or was already consumed");
    }

    SubmitResult result = SubmitResult::Accepted;
    {
        std::lock_guard lock(queueMutex_);
        if (shuttingDown_) {
            --reservations_;
            --inFlight_;
            reserved_.store(reservations_, std::memory_order_relaxed);
            rejected_.fetch_add(1, std::memory_order_relaxed);
            reservation.consume();
            result = SubmitResult::ShuttingDown;
        } else {
            // queue_.push_back may allocate. Leave the reservation live until it
            // succeeds so its destructor releases the in-flight slot on throw.
            queue_.push_back(std::move(job));
            --reservations_;
            reserved_.store(reservations_, std::memory_order_relaxed);
            accepted_.fetch_add(1, std::memory_order_relaxed);
            queued_.store(queue_.size(), std::memory_order_relaxed);
            reservation.consume();
        }
    }

    reservationsCondition_.notify_all();
    publishStats();
    if (result == SubmitResult::Accepted) {
        queueCondition_.notify_one();
    }
    return result;
}

void AudioLoadExecutor::shutdown() {
    std::deque<Job> discardedJobs;
    stopRequested_.store(true, std::memory_order_release);
    {
        std::unique_lock lock(queueMutex_);
        if (shuttingDown_) {
            return;
        }
        accepting_ = false;
        reservationsCondition_.wait(lock, [this]() { return reservations_ == 0; });
        shuttingDown_ = true;
        discardedJobs.swap(queue_);
        inFlight_ -= discardedJobs.size();
        queued_.store(0, std::memory_order_relaxed);
        cancelled_.fetch_add(discardedJobs.size(), std::memory_order_relaxed);
    }

    for (const auto &job : discardedJobs) {
        notifyCancelledJob(job, CancellationReason::Shutdown);
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
        .reserved = reserved_.load(std::memory_order_relaxed),
        .accepted = accepted_.load(std::memory_order_relaxed),
        .completed = completed_.load(std::memory_order_relaxed),
        .rejected = rejected_.load(std::memory_order_relaxed),
        .cancelled = cancelled_.load(std::memory_order_relaxed),
        .failed = failed_.load(std::memory_order_relaxed),
    };
}

std::vector<AudioLoadExecutor::Job> AudioLoadExecutor::takeCancelledJobsLocked() {
    std::vector<Job> discardedJobs;
    std::size_t checked = 0;
    for (auto iterator = queue_.begin(); iterator != queue_.end() && checked < MAX_CANCELLATION_CHECKS_PER_ADMISSION;) {
        ++checked;
        bool cancelled = false;
        if (iterator->isCancelled) {
            try {
                cancelled = iterator->isCancelled();
            } catch (...) {
                // The worker's top-level exception envelope will route a broken
                // predicate through its failure path.
            }
        }
        if (cancelled) {
            discardedJobs.push_back(std::move(*iterator));
            iterator = queue_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return discardedJobs;
}

void AudioLoadExecutor::enqueueCancellationNotificationsLocked(std::vector<Job> &jobs, CancellationReason reason) {
    for (auto &job : jobs) {
        if (!job.onCancelled) {
            continue;
        }
        // Telemetry notification work is also bounded. If a malicious request
        // flood outruns the loader workers, aggregate cancellation counters stay
        // correct without growing a second unbounded queue.
        if (cancellationNotifications_.size() >= queueCapacity_) {
            break;
        }
        cancellationNotifications_.push_back(
            {.id = std::move(job.id), .reason = reason, .callback = std::move(job.onCancelled)});
    }
}

void AudioLoadExecutor::notifyCancelledJob(const Job &job, CancellationReason reason) {
    if (!job.onCancelled) {
        return;
    }
    try {
        job.onCancelled(reason);
    } catch (const std::exception &ex) {
        spdlog::error("RTP audio load job '{}' cancellation handler threw: {}", job.id, ex.what());
    } catch (...) {
        spdlog::error("RTP audio load job '{}' cancellation handler threw an unknown exception", job.id);
    }
}

void AudioLoadExecutor::processCancellationNotification(CancellationNotification notification) {
    if (!notification.callback) {
        return;
    }
    try {
        notification.callback(notification.reason);
    } catch (const std::exception &ex) {
        spdlog::error("RTP audio load job '{}' cancellation handler threw: {}", notification.id, ex.what());
    } catch (...) {
        spdlog::error("RTP audio load job '{}' cancellation handler threw an unknown exception", notification.id);
    }
}

void AudioLoadExecutor::releaseReservation() {
    {
        std::lock_guard lock(queueMutex_);
        if (reservations_ == 0 || inFlight_ == 0) {
            spdlog::critical("RTP audio loader reservation accounting underflow");
            return;
        }
        --reservations_;
        --inFlight_;
        reserved_.store(reservations_, std::memory_order_relaxed);
    }
    reservationsCondition_.notify_all();
    publishStats();
}

void AudioLoadExecutor::workerLoop(std::size_t workerIndex) {
    workersStart_.wait();

    while (true) {
        Job job;
        CancellationNotification cancellationNotification;
        bool hasCancellationNotification = false;
        {
            std::unique_lock lock(queueMutex_);
            try {
                queueCondition_.wait(
                    lock, [this]() { return shuttingDown_ || !cancellationNotifications_.empty() || !queue_.empty(); });
            } catch (const std::exception &ex) {
                spdlog::critical("RTP audio loader worker {} queue wait failed: {}", workerIndex, ex.what());
                return;
            } catch (...) {
                spdlog::critical("RTP audio loader worker {} queue wait failed with an unknown exception", workerIndex);
                return;
            }
            if (!cancellationNotifications_.empty()) {
                cancellationNotification = std::move(cancellationNotifications_.front());
                cancellationNotifications_.pop_front();
                hasCancellationNotification = true;
            } else if (shuttingDown_) {
                return;
            } else {
                job = std::move(queue_.front());
                queue_.pop_front();
                queued_.store(queue_.size(), std::memory_order_relaxed);
                active_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (hasCancellationNotification) {
            processCancellationNotification(std::move(cancellationNotification));
            continue;
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
            notifyCancelledJob(job, CancellationReason::SessionCancelled);
        }
        {
            std::lock_guard lock(queueMutex_);
            active_.fetch_sub(1, std::memory_order_relaxed);
            if (inFlight_ == 0) {
                spdlog::critical("RTP audio loader in-flight accounting underflow");
            } else {
                --inFlight_;
            }
        }
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
