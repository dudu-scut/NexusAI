#pragma once

#include "agent_rpc/server/auth_service.h"

#include "agent_rpc/common/redis_client.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace agent_rpc {
namespace server {

/**
 * @brief Cache-aside session cache with logical invalidation (P26 T2).
 *
 * The read-modify-write of the auth cache lives here — a header-only helper
 * deliberately separate from AuthServiceImpl so the service stays Redis-free
 * (contract-locked by test_local_auth_contract) while the interceptor AND the
 * ValidateToken RPC share one code path. RedisClient is injected once at
 * server assembly and stored statically, mirroring the pre-P26 interceptor
 * wiring.
 *
 * Redis keys (see memory_service.h key classification; auth:session is
 * cache-only, auth:deny / auth:epoch are transient revocation primitives):
 *   - auth:session:<token_hash> → {user_id, username, role, epoch, expires_at}
 *     TTL = the session's true remaining lifetime (SETEX at refill), never a
 *     fixed approximation. The epoch inside must match auth:epoch:<owner_id>
 *     or the entry is treated as a miss and re-validated against PG.
 *   - auth:deny:<token_hash> → short-lived (60s) session-level revocation
 *     marker written by Logout. Covers the window between the cache DEL and
 *     a concurrent in-flight miss refill; it expires quickly and never
 *     accumulates.
 *   - auth:epoch:<owner_id> → per-user monotonic counter (INCR) for
 *     user-level revocation (ban / demote / password change). No producer
 *     exists yet — management RPCs will INCR here. An absent key reads as 0.
 *
 * Failure semantics: Redis down or a command failure degrades to the
 * authoritative PostgreSQL JOIN check. Only false negatives (a valid user
 * refused once) are allowed — a cache entry is only ever written after PG
 * validated the token, so PG remains the final judge.
 */
class AuthCache {
public:
    static constexpr int kDenyTtlSeconds = 60;

    static std::string sessionKey(const std::string& token_hash) {
        return "auth:session:" + token_hash;
    }

    static std::string denyKey(const std::string& token_hash) {
        return "auth:deny:" + token_hash;
    }

    static std::string epochKey(const std::string& owner_id) {
        return "auth:epoch:" + owner_id;
    }

    /** Assembly-point injection (called by AuthInterceptor::setRedisClient). */
    static void setRedisClient(common::RedisClient* redis) {
        s_redis_ = redis;
    }

    static common::RedisClient* redis() {
        return s_redis_;
    }

    /**
     * Revocation bus write point (session-level): drop the cached entry and
     * arm the short-lived deny marker so a concurrent in-flight miss refill
     * cannot resurrect a revoked session. PG revocation is the caller's
     * responsibility (Logout); this only guards the cache window.
     */
    static bool markRevoked(const std::string& token_hash) {
        if (s_redis_ == nullptr || !s_redis_->isConnected()) {
            // No cache to guard; PG revocation alone is authoritative.
            return false;
        }
        s_redis_->del(sessionKey(token_hash));
        return s_redis_->setex(denyKey(token_hash), kDenyTtlSeconds, "1");
    }

    /**
     * Validate a token through the cache (P26 T2 check path):
     *   1. deny hit → invalid without touching PG (revocation already wrote
     *      PG first; refusing here is the allowed false-negative direction);
     *   2. session hit with matching epoch and unexpired payload → valid,
     *      zero PostgreSQL round-trips;
     *   3. otherwise → authoritative validateTokenWithStatus, then refill
     *      the cache with the current epoch and the true remaining TTL.
     */
    static AuthValidationResult validateTokenCached(
        AuthServiceImpl& auth_service,
        const std::string& token,
        std::string& user_id,
        std::string& username,
        std::string& role,
        std::int64_t& expires_at) {
        user_id.clear();
        username.clear();
        role.clear();
        expires_at = 0;

        const std::string token_hash = AuthServiceImpl::hashToken(token);
        if (token_hash.empty()) {
            return AuthValidationResult::kInvalid;
        }

        const auto fallback = [&]() {
            return auth_service.validateTokenWithStatus(
                token, user_id, username, role, expires_at);
        };

        common::RedisClient* redis = s_redis_;
        if (redis == nullptr || !redis->isConnected()) {
            // Degrade to the authoritative PG check (fail-safe).
            return fallback();
        }

        std::vector<std::string> values;
        if (!redis->mget({denyKey(token_hash), sessionKey(token_hash)}, values) ||
            values.size() != 2) {
            return fallback();
        }
        if (!values[0].empty()) {
            return AuthValidationResult::kInvalid;
        }

        if (!values[1].empty()) {
            try {
                const auto payload = nlohmann::json::parse(values[1]);
                const std::string cached_user_id = payload.value("user_id", "");
                if (!cached_user_id.empty()) {
                    // Explicit int64_t: expires_at/epoch are Unix seconds —
                    // the int template default would narrow near 2038.
                    const std::int64_t payload_epoch =
                        payload.value<std::int64_t>("epoch", 0);
                    const std::int64_t payload_expires =
                        payload.value<std::int64_t>("expires_at", 0);
                    const std::string cached_username =
                        payload.value("username", "");
                    const std::string cached_role = payload.value("role", "");
                    const std::int64_t current_epoch = readEpoch(redis, cached_user_id);
                    const bool epoch_matches = payload_epoch == current_epoch;
                    const bool not_expired = payload_expires > nowEpoch();
                    if (epoch_matches && not_expired &&
                        !cached_username.empty() && !cached_role.empty()) {
                        user_id = cached_user_id;
                        username = cached_username;
                        role = cached_role;
                        expires_at = payload_expires;
                        return AuthValidationResult::kValid;
                    }
                    // Epoch mismatch, stale payload or missing identity →
                    // re-validate against PG below and refill. Refusing on a
                    // malformed-but-parseable entry would hard-block a valid
                    // user for the whole remaining TTL (a cache entry is
                    // only ever written after PG validated it, so refilling
                    // cannot resurrect anything).
                }
            } catch (const nlohmann::json::exception&) {
                // Corrupt payload → treat as a miss and refill.
            }
        }

        const AuthValidationResult result = fallback();
        if (result == AuthValidationResult::kValid) {
            refill(redis, token_hash, user_id, username, role, expires_at);
        }
        return result;
    }

private:
    static std::int64_t nowEpoch() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static std::int64_t readEpoch(common::RedisClient* redis,
                                  const std::string& owner_id) {
        std::string raw;
        if (redis->get(epochKey(owner_id), raw) && !raw.empty()) {
            try {
                return std::stoll(raw);
            } catch (const std::exception&) {
                return 0;
            }
        }
        // Absent epoch key reads as 0: no user-level revocation ever fired.
        return 0;
    }

    static void refill(common::RedisClient* redis,
                       const std::string& token_hash,
                       const std::string& user_id,
                       const std::string& username,
                       const std::string& role,
                       std::int64_t expires_at) {
        if (user_id.empty() || redis == nullptr || !redis->isConnected()) {
            return;
        }
        const std::int64_t ttl = expires_at - nowEpoch();
        if (ttl <= 0) {
            return;  // Session already expired; PG refused it anyway.
        }
        // Persist the current epoch so the payload fails closed after any
        // future INCR (mismatch → re-validation against PG). TTL is the
        // session's true remaining lifetime (≤ 24h, int-safe).
        const nlohmann::json payload = {
            {"user_id", user_id},
            {"username", username},
            {"role", role},
            {"epoch", readEpoch(redis, user_id)},
            {"expires_at", expires_at}};
        redis->setex(sessionKey(token_hash), static_cast<int>(ttl),
                     payload.dump());
    }

    static common::RedisClient* s_redis_;
};

inline common::RedisClient* AuthCache::s_redis_ = nullptr;

}  // namespace server
}  // namespace agent_rpc
