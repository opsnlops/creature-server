#pragma once

#include <atomic>
#include <memory>

namespace creatures::rtp {

/**
 * Bounds standalone RTP load/playback to one active lifecycle.
 *
 * The reservation is transferred from the request to the loader, then along
 * the self-scheduling playback chain. Request bursts therefore cannot create
 * unbounded loader threads or pending standalone playback chains.
 */
class StandaloneRtpAdmission {
  public:
    class Reservation {
      public:
        ~Reservation() {
            if (admission_) {
                admission_->release();
            }
        }

        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;

      private:
        friend class StandaloneRtpAdmission;
        explicit Reservation(StandaloneRtpAdmission *admission) : admission_(admission) {}

        StandaloneRtpAdmission *admission_;
    };

    [[nodiscard]] std::shared_ptr<Reservation> tryAcquire() {
        bool expected = false;
        if (!reserved_.compare_exchange_strong(expected, true)) {
            return nullptr;
        }
        return std::shared_ptr<Reservation>(new Reservation(this));
    }

    [[nodiscard]] bool isReserved() const { return reserved_.load(); }

  private:
    void release() { reserved_.store(false); }

    std::atomic<bool> reserved_{false};
};

inline StandaloneRtpAdmission &standaloneRtpAdmission() {
    static StandaloneRtpAdmission admission;
    return admission;
}

} // namespace creatures::rtp
