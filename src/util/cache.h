
#pragma once

#include <iostream>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

namespace creatures {

    template <typename Key, typename Value>
    class ObjectCache {
    public:

        ObjectCache() = default;
        ~ObjectCache() = default;

        /**
         * Put an item into the cache, replacing what's there if it already exists
         *
         * @param key the key
         * @param value the thing to put in
         */
        void put(const Key& key, std::shared_ptr<Value> value) {
            std::unique_lock lock(mutex_);
            map_.erase(key);
            map_[key] = value;
        }

        // Overloaded put method that takes a normal object and wraps it into a shared_ptr
        void put(const Key& key, const Value& value) {
            std::unique_lock lock(mutex_);
            map_.erase(key);
            map_[key] = std::make_shared<Value>(value);
        }

        std::shared_ptr<Value> get(const Key& key) {
            std::shared_lock lock(mutex_);
            auto it = map_.find(key);
            if (it == map_.end()) {
                throw std::out_of_range("Key not found");
            }
            return it->second;
        }

        void remove(const Key& key) {
            std::unique_lock lock(mutex_);
            map_.erase(key);
        }

    private:
        std::unordered_map<Key, std::shared_ptr<Value>> map_;
        mutable std::shared_mutex mutex_;
    };

}