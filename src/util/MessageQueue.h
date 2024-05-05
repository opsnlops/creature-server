
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>

namespace creatures {


    /**
     * A simple thread-safe message queue
     *
     * Used for passing messages around between threads in order
     *
     * @tparam T
     */
    template<typename T>
    class MessageQueue {
    public:
        void push(T message) {
            std::lock_guard<std::mutex> lock(mtx);
            queue.push_back(std::move(message));
            cond.notify_one(); // Hop, hop! A new message is here!
        }

        T pop() {
            std::unique_lock<std::mutex> lock(mtx);
            cond.wait(lock, [this] { return !queue.empty(); }); // Wait patiently like a bunny in a burrow
            T msg = std::move(queue.front());
            queue.pop_front();
            return msg;
        }

    private:
        std::mutex mtx;
        std::condition_variable cond;
        std::deque<T> queue;
    };
}