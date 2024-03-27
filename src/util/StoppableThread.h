
#pragma once

#include <atomic>
#include <thread>

namespace creatures {

    /**
     * A simple thread class that can be stopped
     */
    class StoppableThread {
    public:
        StoppableThread() : stop_requested(false) {}
        virtual ~StoppableThread() {
            if (thread.joinable()) {
                thread.join();
            }
        }

        bool isThreadJoinable() {
            return thread.joinable();
        }

        virtual void start() {
            thread = std::thread(&StoppableThread::run, this);
        }

        void shutdown() {
            stop_requested.store(true);
        }

    protected:
        virtual void run() = 0; // This is the method that will be called when the thread starts

        std::atomic<bool> stop_requested;

    private:
        std::thread thread;
    };

} // namespace creatures

