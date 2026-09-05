#pragma once

#include <hiredis/hiredis.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace agent_rpc {
namespace common {

/**
 * @brief Thin wrapper around hiredis for Redis operations.
 *
 * Thread-safe: uses a single connection protected by a mutex.
 * Sufficient for the synchronous gRPC server model.
 */
class RedisClient {
public:
    RedisClient();
    ~RedisClient();

    /** Connect to Redis server. Returns true on success. */
    bool connect(const std::string& host = "127.0.0.1",
                 int port = 6379,
                 int timeout_ms = 1000);

    /** Disconnect from Redis. */
    void disconnect();

    /** Check if connected. */
    bool isConnected() const;

    // ========================================================================
    // String operations
    // ========================================================================

    bool set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& value);
    bool del(const std::string& key);
    bool exists(const std::string& key);

    /** Set key with TTL in seconds. */
    bool setex(const std::string& key, int ttl_seconds, const std::string& value);

    // ========================================================================
    // Hash operations
    // ========================================================================

    bool hset(const std::string& key, const std::string& field,
              const std::string& value);
    bool hget(const std::string& key, const std::string& field,
              std::string& value);
    bool hgetall(const std::string& key,
                 std::map<std::string, std::string>& result);
    bool hdel(const std::string& key, const std::string& field);

    /** Set field only if it does not exist. Returns true if field was newly set. */
    bool hsetnx(const std::string& key, const std::string& field,
                const std::string& value);

    // ========================================================================
    // Set operations
    // ========================================================================

    /** Add a member to a set (C2 direction 3: preference-conflict candidates). */
    bool sadd(const std::string& key, const std::string& member);

    // ========================================================================
    // List operations
    // ========================================================================

    /** Push value to the tail of a list. */
    bool rpush(const std::string& key, const std::string& value);

    /** Get elements from a list. Negative stop = end of list. */
    bool lrange(const std::string& key, int start, int stop,
                std::vector<std::string>& result);

    /** Trim list to [start, stop] range. */
    bool ltrim(const std::string& key, int start, int stop);

    /** Pop the head of a list into result. Returns true only when a value
     *  was popped; false on error or when the list is empty. */
    bool lpop(const std::string& key, std::string& result);

    /** Set TTL on a key (seconds). */
    bool expire(const std::string& key, int seconds);

    /** Atomically increment key by increment. Returns the new value via result. */
    bool incrby(const std::string& key, int64_t increment, int64_t& result);

private:
    /** Attempt reconnect if connection is lost. Must be called with mutex_ held. */
    bool ensureConnected();

    redisContext* ctx_;
    mutable std::mutex mutex_;

    // Stored for lazy reconnection
    std::string host_;
    int port_ = 6379;
    int timeout_ms_ = 1000;
};

}  // namespace common
}  // namespace agent_rpc
