//
// Created by Kotarou on 2022/4/10.
//

#ifndef YADDNSC_CACHE_H
#define YADDNSC_CACHE_H

#include <map>
#include <mutex>
#include <ctime>
#include <chrono>
#include <optional>
#include <functional>
#include <string_view>

template<typename T>
class Cache {
public:
    struct cache_entity_t {
        T data;
        std::time_t expire;
    };

    void put(const std::string &key, T data, int expire) {
        std::unique_lock<std::mutex> lock(_write_mutex);
        _cache.insert_or_assign(key, cache_entity_t{data, std::time(nullptr) + expire});
    }

    std::optional<T> get(std::string_view key) {
        std::unique_lock<std::mutex> lock(_read_mutex);
        if (contains(key)) {
            auto data = _cache.at(key.data());
            auto now = std::time(nullptr);
            if (data.expire - now > 0) {
                return data.data;
            } else {
                erase(key.data());
            }
        }

        return std::nullopt;
    }

    T get(const std::string &key, const std::function<T()> &producer, int expire) {
        std::unique_lock<std::mutex> lock_r(_read_mutex);
        if (contains(key)) {
            auto data = _cache.at(key.data());
            auto now = std::time(nullptr);

            if (data.expire - now > 0) {
                return data.data;
            } else {
                erase(key);
            }
        }

        auto data = producer();
        put(key, data, expire);

        return data;
    }

    [[nodiscard]] bool contains(std::string_view key) const {
        return _cache.find(key.data()) != _cache.end();
    }

private:
    size_t erase(const std::string &key) {
        std::unique_lock<std::mutex> lock(_write_mutex);
        return _cache.erase(key);
    }

private:
    std::map<std::string, cache_entity_t> _cache;

    std::mutex _read_mutex;

    std::mutex _write_mutex;
};

#endif //YADDNSC_CACHE_H
