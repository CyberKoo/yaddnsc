//
// Created by Kotarou on 2026/6/21.
//

#ifndef YADDNSC_UTIL_CACHE_H
#define YADDNSC_UTIL_CACHE_H

#include <chrono>
#include <concepts>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>

namespace Util {
    /**
     * A generic thread-safe TTL (time-to-live) cache.
     *
     * @tparam Key   The key type (must be equality-comparable and hashable).
     * @tparam Value The value type (must be copy-constructible).
     */
    template<typename Key, typename Value> requires std::equality_comparable<Key> && std::copy_constructible<Value>
    class TtlCache {
    public:
        using Clock = std::chrono::steady_clock;

        /**
         * @param ttl  How long an entry remains valid after insertion.
         */
        explicit TtlCache(std::chrono::nanoseconds ttl) : ttl_(ttl) {
        }

        /**
         * Retrieve a value by key.  Returns std::nullopt when the key
         * is not present or the cached entry has expired.
         */
        std::optional<Value> get(const Key &key) {
            std::lock_guard lock(mutex_);
            auto it = map_.find(key);
            if (it == map_.end()) {
                return std::nullopt;
            }
            if (expired(it->second)) {
                map_.erase(it);
                return std::nullopt;
            }
            return it->second.value;
        }

        /**
         * Store a value under the given key, resetting its TTL.
         */
        void set(const Key &key, Value value) {
            std::lock_guard lock(mutex_);
            map_[key] = Entry{Clock::now(), std::move(value)};
        }

        /**
         * Returns true if the key exists and is not expired.
         */
        bool contains(const Key &key) {
            return get(key).has_value();
        }

        /**
         * Remove a single entry.
         */
        void remove(const Key &key) {
            std::lock_guard lock(mutex_);
            map_.erase(key);
        }

        /**
         * Remove all entries.
         */
        void clear() {
            std::lock_guard lock(mutex_);
            map_.clear();
        }

        /**
         * Return the cached value for `key`, or compute-and-cache it
         * via the provided factory if absent / expired.
         */
        template<std::invocable<> Fn> requires std::same_as < std::invoke_result_t < Fn >

        ,
        Value
        >
        Value get_or_compute(const Key &key, Fn &&factory) {
            if (auto cached = get(key)) {
                return std::move(*cached);
            }
            Value value = std::invoke(std::forward<Fn>(factory));
            set(key, value);
            return value;
        }

        /**
         * Invalidate all entries matching a predicate.
         * The predicate receives (const Key &, const Value &) and returns bool.
         */
        template<typename Pred> requires std::predicate<Pred, const Key &, const Value &>
        void invalidate_if(Pred &&pred) {
            std::lock_guard lock(mutex_);
            for (auto it = map_.begin(); it != map_.end();) {
                if (pred(it->first, it->second.value)) {
                    it = map_.erase(it);
                } else {
                    ++it;
                }
            }
        }

    private:
        struct Entry {
            Clock::time_point timestamp;
            Value value;
        };

        bool expired(const Entry &entry) const {
            return (Clock::now() - entry.timestamp) >= ttl_;
        }

        std::chrono::nanoseconds ttl_;
        std::mutex mutex_;
        std::unordered_map<Key, Entry> map_;
    };
} // namespace Util

#endif // YADDNSC_UTIL_CACHE_H
