#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/logger.h"

namespace agent_rpc {
namespace common {

RedisClient::RedisClient() : ctx_(nullptr) {}

RedisClient::~RedisClient() {
    disconnect();
}

bool RedisClient::connect(const std::string& host, int port, int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Store params for lazy reconnection
    host_ = host;
    port_ = port;
    timeout_ms_ = timeout_ms;

    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ctx_ = redisConnectWithTimeout(host.c_str(), port, tv);
    if (!ctx_ || ctx_->err) {
        if (ctx_) {
            LOG_ERROR("Redis connect failed: " + std::string(ctx_->errstr));
            redisFree(ctx_);
            ctx_ = nullptr;
        } else {
            LOG_ERROR("Redis connect failed: can't allocate redis context");
        }
        return false;
    }

    // Command timeout: the connect timeout above only bounds the handshake.
    // Without redisSetTimeout a stalled Redis (process alive, network
    // partition) blocks redisCommand() reads indefinitely on the RPC thread.
    // The same timeout bounds every command; failures degrade per-call.
    if (redisSetTimeout(ctx_, tv) != REDIS_OK) {
        LOG_WARN("Redis command timeout not applied, commands may block");
    }

    LOG_INFO("Redis connected to " + host + ":" + std::to_string(port));
    return true;
}

void RedisClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisClient::ensureConnected() {
    // Caller must hold mutex_
    if (ctx_ && !ctx_->err) return true;

    // Free broken context
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }

    if (host_.empty()) return false;  // never connected via connect()

    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;

    ctx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (!ctx_ || ctx_->err) {
        if (ctx_) {
            redisFree(ctx_);
            ctx_ = nullptr;
        }
        return false;
    }
    // Re-applied on every reconnect so lazy reconnection never drops the
    // command-read timeout (see connect()).
    if (redisSetTimeout(ctx_, tv) != REDIS_OK) {
        LOG_WARN("Redis command timeout not applied on reconnect");
    }
    return true;
}

bool RedisClient::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ctx_ != nullptr && !ctx_->err;
}

// ============================================================================
// String operations
// ============================================================================

bool RedisClient::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SET %s %b", key.c_str(), value.data(), value.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::get(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", key.c_str()));
    if (!reply) return false;

    if (reply->type == REDIS_REPLY_STRING) {
        value.assign(reply->str, reply->len);
        freeReplyObject(reply);
        return true;
    }
    freeReplyObject(reply);
    return false;
}

bool RedisClient::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "DEL %s", key.c_str()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "EXISTS %s", key.c_str()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::mget(const std::vector<std::string>& keys,
                       std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    values.clear();
    if (keys.empty()) {
        return true;
    }
    if (!ensureConnected()) return false;

    // redisCommandArgv keeps binary-safe arguments and avoids any format-
    // string / whitespace hazard when keys are concatenated into one line.
    std::vector<const char*> argv;
    argv.reserve(keys.size() + 1);
    argv.push_back("MGET");
    for (const auto& key : keys) {
        argv.push_back(key.c_str());
    }
    auto* reply = static_cast<redisReply*>(redisCommandArgv(
        ctx_, static_cast<int>(argv.size()), argv.data(), nullptr));
    if (!reply) return false;

    bool ok = (reply->type == REDIS_REPLY_ARRAY);
    if (ok) {
        values.reserve(reply->elements);
        for (std::size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i]->type == REDIS_REPLY_STRING) {
                values.emplace_back(reply->element[i]->str,
                                    reply->element[i]->len);
            } else {
                values.emplace_back();  // nil key → empty value
            }
        }
    }
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::setex(const std::string& key, int ttl_seconds,
                         const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SETEX %s %d %b",
                     key.c_str(), ttl_seconds, value.data(), value.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);
    return ok;
}

// ============================================================================
// Hash operations
// ============================================================================

bool RedisClient::hset(const std::string& key, const std::string& field,
                        const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HSET %s %s %b",
                     key.c_str(), field.c_str(), value.data(), value.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::hget(const std::string& key, const std::string& field,
                        std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HGET %s %s", key.c_str(), field.c_str()));
    if (!reply) return false;

    if (reply->type == REDIS_REPLY_STRING) {
        value.assign(reply->str, reply->len);
        freeReplyObject(reply);
        return true;
    }
    freeReplyObject(reply);
    return false;
}

bool RedisClient::hgetall(const std::string& key,
                           std::map<std::string, std::string>& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result.clear();
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HGETALL %s", key.c_str()));
    if (!reply) return false;

    bool ok = (reply->type == REDIS_REPLY_ARRAY);
    if (ok) {
        for (size_t i = 0; i + 1 < reply->elements; i += 2) {
            if (reply->element[i]->type == REDIS_REPLY_STRING &&
                reply->element[i + 1]->type == REDIS_REPLY_STRING) {
                std::string k(reply->element[i]->str, reply->element[i]->len);
                std::string v(reply->element[i + 1]->str, reply->element[i + 1]->len);
                result[k] = v;
            }
        }
    }
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::hdel(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HDEL %s %s", key.c_str(), field.c_str()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::hsetnx(const std::string& key, const std::string& field,
                          const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HSETNX %s %s %b",
                     key.c_str(), field.c_str(), value.data(), value.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    return ok;
}

// ============================================================================
// Set operations
// ============================================================================

bool RedisClient::sadd(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SADD %s %b",
                     key.c_str(), member.data(), member.size()));
    if (!reply) return false;
    // A WRONGTYPE reply (key shadowed by another structure) must not look
    // like a silent success — log it so the conflict detection loss is
    // visible.
    if (reply->type != REDIS_REPLY_INTEGER) {
        LOG_WARN("SADD failed for key " + key + " (unexpected reply type)");
    }
    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return ok;
}

// ============================================================================
// List operations
// ============================================================================

bool RedisClient::rpush(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "RPUSH %s %b",
                     key.c_str(), value.data(), value.size()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::lrange(const std::string& key, int start, int stop,
                          std::vector<std::string>& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result.clear();
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "LRANGE %s %d %d", key.c_str(), start, stop));
    if (!reply) return false;

    bool ok = (reply->type == REDIS_REPLY_ARRAY);
    if (ok) {
        for (size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i]->type == REDIS_REPLY_STRING) {
                result.emplace_back(reply->element[i]->str, reply->element[i]->len);
            }
        }
    }
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::lpop(const std::string& key, std::string& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    result.clear();
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "LPOP %s", key.c_str()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_STRING);
    if (ok) {
        result.assign(reply->str, reply->len);
    }
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::ltrim(const std::string& key, int start, int stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "LTRIM %s %d %d", key.c_str(), start, stop));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_STATUS);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::expire(const std::string& key, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "EXPIRE %s %d", key.c_str(), seconds));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return ok;
}

bool RedisClient::ttl(const std::string& key, std::int64_t& seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "TTL %s", key.c_str()));
    if (!reply) return false;

    if (reply->type == REDIS_REPLY_INTEGER) {
        seconds = reply->integer;
        freeReplyObject(reply);
        return true;
    }
    freeReplyObject(reply);
    return false;
}

bool RedisClient::incrby(const std::string& key, int64_t increment,
                          int64_t& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureConnected()) return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "INCRBY %s %lld", key.c_str(),
                     static_cast<long long>(increment)));
    if (!reply) return false;

    if (reply->type == REDIS_REPLY_INTEGER) {
        result = reply->integer;
        freeReplyObject(reply);
        return true;
    }
    freeReplyObject(reply);
    return false;
}

}  // namespace common
}  // namespace agent_rpc
