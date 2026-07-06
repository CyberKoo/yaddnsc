//
// Created by Kotarou on 2026/6/21.
//

#ifndef YADDNSC_UTIL_CACHE_H
#define YADDNSC_UTIL_CACHE_H

#include <mutex>
#include <future>
#include <memory>
#include <chrono>
#include <concepts>
#include <optional>
#include <functional>
#include <type_traits>
#include <unordered_map>

namespace Utils::Cache {
    /// A generic thread-safe TTL (time-to-live) cache.
    ///
    /// Supports get/set/contains/remove/clear with automatic expiry.
    /// The get_or_compute() method provides thundering-herd protection:
    /// only one thread computes the value for a given key while others
    /// wait on a shared_future.
    ///
    /// @tparam Key   The key type (must be equality-comparable and hashable).
    /// @tparam Value The value type (must be copy-constructible).
    template<typename Key, typename Value> requires std::equality_comparable<Key> && std::copy_constructible<Value>
    class TtlCache {
    public:
        using Clock = std::chrono::steady_clock;

        /// Construct with a TTL duration.
        /// @param ttl  How long an entry remains valid after insertion.
        explicit TtlCache(std::chrono::nanoseconds ttl) : ttl_(ttl) {
        }

        /// Retrieve a value by key.
        /// @param key  The lookup key.
        /// @return     The cached value, or std::nullopt if missing or expired.
        std::optional<Value> get(const Key &key) {
            std::lock_guard lock(mutex_);
            return do_get(key);
        }

        /// Store a value under the given key, resetting its TTL.
        /// @param key    The cache key.
        /// @param value  The value to cache.
        void set(const Key &key, Value value) {
            std::lock_guard lock(mutex_);
            map_.insert_or_assign(key, Entry{Clock::now(), std::move(value)});
        }

        /// Check whether a key exists and is not expired.
        /// @return  true if the key is present and fresh.
        bool contains(const Key &key) {
            std::lock_guard lock(mutex_);
            return do_get(key).has_value();
        }

        /// Remove a single entry from the cache.
        ///
        /// If a computation for this key is already in flight it is
        /// allowed to finish; its result will still be stored so that
        /// waiters do not recompute unnecessarily.
        void remove(const Key &key) {
            std::lock_guard lock(mutex_);
            map_.erase(key);
        }

        /// Remove all entries from the cache.
        ///
        /// In-flight computations are not interrupted, but they will
        /// still receive the same thundering-herd deduplication.
        void clear() {
            std::lock_guard lock(mutex_);
            map_.clear();
        }

        /// Return the cached value for `key`, or compute-and-cache it
        /// via the provided factory if absent / expired.
        ///
        /// The factory runs **outside** the internal mutex, so heavy
        /// computations do not block other cache operations.
        ///
        /// If multiple threads miss the same key simultaneously, only
        /// one thread invokes the factory; the others block on a
        /// shared_future and receive the same result once it is ready
        /// (thundering-herd protection).
        ///
        /// @tparam Fn  Invocable returning Value.
        /// @param key       The cache key.
        /// @param factory   Factory function to compute the value if not cached.
        /// @return          The cached or freshly computed value.
        template<std::invocable<> Fn>
            requires std::same_as<std::invoke_result_t<Fn>, Value>
        Value get_or_compute(const Key &key, Fn &&factory) {
            std::unique_lock lock(mutex_);

            // Fast path: value is already cached and fresh
            if (auto cached = do_get(key)) {
                return *std::move(cached);
            }

            // Another thread is already computing this key — wait for it
            if (auto it = pending_.find(key); it != pending_.end()) {
                auto future = it->second;
                lock.unlock();
                return future.get();
            }

            // We are responsible for computing the value
            auto promise = std::make_shared<std::promise<Value>>();
            auto shared_future = promise->get_future().share();
            pending_[key] = shared_future;
            lock.unlock();

            // Compute outside the mutex — other cache operations are not blocked
            try {
                Value value = std::invoke(std::forward<Fn>(factory));
                promise->set_value(value);

                // Store the computed result back into the cache
                lock.lock();
                pending_.erase(key);
                map_.insert_or_assign(key, Entry{Clock::now(), std::move(value)});
                return value;
            } catch (...) {
                // Propagate the exception to all waiters
                promise->set_exception(std::current_exception());
                lock.lock();
                pending_.erase(key);
                throw;
            }
        }

        /// Invalidate all entries matching a predicate.
        /// @tparam Pred  Predicate `bool(const Key &, const Value &)`.
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

        /// Return the number of entries in the cache.
        [[nodiscard]] std::size_t size() const {
            std::lock_guard lock(mutex_);
            return map_.size();
        }

        /// Check if the cache contains no entries.
        [[nodiscard]] bool empty() const {
            std::lock_guard lock(mutex_);
            return map_.empty();
        }

    private:
        struct Entry {
            Clock::time_point timestamp;
            Value value;
        };

        // Internal helper: caller must already hold mutex_.
        std::optional<Value> do_get(const Key &key) {
            auto it = map_.find(key);
            if (it == map_.end() || expired(it->second)) {
                if (it != map_.end()) {
                    map_.erase(it);
                }
                return std::nullopt;
            }
            return it->second.value;
        }

        bool expired(const Entry &entry) const {
            return (Clock::now() - entry.timestamp) >= ttl_;
        }

        std::chrono::nanoseconds ttl_;
        mutable std::mutex mutex_;
        std::unordered_map<Key, Entry> map_;
        std::unordered_map<Key, std::shared_future<Value>> pending_;
    };
} // namespace Utils::Cache

#endif // YADDNSC_UTIL_CACHE_H
