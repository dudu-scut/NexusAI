#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/server/auth_cache.h"
#include "agent_rpc/server/auth_service.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/trace_context.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <cstddef>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <vector>

namespace agent_rpc {
namespace server {
namespace {

constexpr std::size_t kMinBearerTokenLength = 64;
constexpr std::size_t kMaxBearerTokenLength = 256;
constexpr std::size_t kMaxTraceIdLength = 128;

bool isAsciiHex(const unsigned char character) {
    return (character >= static_cast<unsigned char>('0') &&
            character <= static_cast<unsigned char>('9')) ||
           (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('f')) ||
           (character >= static_cast<unsigned char>('A') &&
            character <= static_cast<unsigned char>('F'));
}

bool isValidBearerAuthorization(const std::string& value) {
    constexpr char kBearerPrefix[] = "Bearer ";
    constexpr std::size_t kBearerPrefixLength = sizeof(kBearerPrefix) - 1;
    if (value.size() <= kBearerPrefixLength ||
        value.compare(0, kBearerPrefixLength, kBearerPrefix) != 0) {
        return false;
    }

    const std::size_t token_length = value.size() - kBearerPrefixLength;
    if (token_length < kMinBearerTokenLength || token_length > kMaxBearerTokenLength) {
        return false;
    }
    for (std::size_t index = kBearerPrefixLength; index < value.size(); ++index) {
        if (!isAsciiHex(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool isValidTraceId(const grpc::string_ref& value) {
    if (value.size() > kMaxTraceIdLength) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value.data()[index]);
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

// P26 T3: HMAC-SHA256 (hex, lowercase) over the shared proxy secret.
std::string hmacSha256Hex(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_length);
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded(digest_length * 2, '0');
    for (unsigned int index = 0; index < digest_length; ++index) {
        encoded[index * 2] = kHex[(digest[index] >> 4) & 0x0f];
        encoded[index * 2 + 1] = kHex[digest[index] & 0x0f];
    }
    return encoded;
}

bool hexDecodeToBytes(const std::string& encoded, std::vector<unsigned char>& bytes) {
    if (encoded.empty() || encoded.size() % 2 != 0) return false;
    bytes.resize(encoded.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = static_cast<unsigned char>(encoded[index * 2]);
        const auto low = static_cast<unsigned char>(encoded[index * 2 + 1]);
        const auto nibble = [](const unsigned char character) -> int {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f') return character - 'a' + 10;
            if (character >= 'A' && character <= 'F') return character - 'A' + 10;
            return -1;
        };
        const int high_value = nibble(high);
        const int low_value = nibble(low);
        if (high_value < 0 || low_value < 0) return false;
        bytes[index] = static_cast<unsigned char>((high_value << 4) | low_value);
    }
    return true;
}

// Constant-time comparison of two hex signatures: a timing side channel on
// the expected HMAC would let an attacker recover the tag byte-by-byte.
bool constantTimeHexEqual(const std::string& left, const std::string& right) {
    std::vector<unsigned char> left_bytes;
    std::vector<unsigned char> right_bytes;
    if (!hexDecodeToBytes(left, left_bytes) ||
        !hexDecodeToBytes(right, right_bytes) ||
        left_bytes.size() != right_bytes.size()) {
        return false;
    }
    return CRYPTO_memcmp(left_bytes.data(), right_bytes.data(),
                         left_bytes.size()) == 0;
}

}  // namespace

thread_local AuthInterceptor::AuthContext AuthInterceptor::tls_auth_;
std::atomic<bool> AuthInterceptor::auth_enabled_{false};
std::atomic<bool> AuthInterceptor::trust_proxy_enabled_{false};
std::string AuthInterceptor::proxy_hmac_secret_;
AuthServiceImpl* AuthInterceptor::auth_service_handle_ = nullptr;

void AuthInterceptor::setRedisClient(common::RedisClient* redis) {
    // P26 T2: the cache read-modify-write moved into the shared AuthCache
    // helper; this accessor forwards so RpcServer wiring stays unchanged.
    AuthCache::setRedisClient(redis);
}

void AuthInterceptor::setTrustedProxy(const std::string& hmac_secret,
                                      bool enabled) {
    // Written once at RpcServer assembly before serving starts; read-only
    // afterwards, so no locking is needed for the concurrent read paths.
    proxy_hmac_secret_ = hmac_secret;
    trust_proxy_enabled_.store(enabled, std::memory_order_relaxed);
}

bool AuthInterceptor::isTrustProxyEnabled() {
    return trust_proxy_enabled_.load(std::memory_order_relaxed);
}

void AuthInterceptor::setAuthService(AuthServiceImpl* auth_service) {
    auth_service_handle_ = auth_service;
}

AuthInterceptor::AuthInterceptor(AuthServiceImpl* auth_service,
                                  const std::string& method_path)
    : auth_service_(auth_service), method_path_(method_path) {}

void AuthInterceptor::Intercept(
    grpc::experimental::InterceptorBatchMethods* methods) {

    if (methods->QueryInterceptionHookPoint(
            grpc::experimental::InterceptionHookPoints::POST_RECV_INITIAL_METADATA)) {

        // Reset thread-local auth state for each new RPC
        tls_auth_ = AuthContext{};

        // Extract trace ID from incoming metadata for distributed tracing
        auto* metadata = methods->GetRecvInitialMetadata();
        if (metadata) {
            auto trace_it = metadata->find("x-trace-id");
            if (trace_it != metadata->end() && trace_it->second.size() != 0 &&
                isValidTraceId(trace_it->second)) {
                tls_auth_.trace_id = std::string(trace_it->second.data(), trace_it->second.size());
            }
            // Cross-process delegation depth (R41): the a2a_adapter sends
            // depth+1 on every delegated call, but nothing consumed the
            // header — the MAX_DEPTH guard only ever counted intra-process
            // adapter calls. Seed this process's TLS counter from the header
            // so the 5-layer limit spans process boundaries.
            auto depth_it = metadata->find("x-delegation-depth");
            if (depth_it != metadata->end() && depth_it->second.size() != 0) {
                int inbound_depth = 0;
                try {
                    inbound_depth = std::stoi(std::string(
                        depth_it->second.data(), depth_it->second.size()));
                } catch (const std::exception&) {
                    inbound_depth = 0;
                }
                // Clamp to the router's MAX_DEPTH band: a forged header
                // must neither bypass the limit (small value resetting the
                // counter) nor wedge every later call (huge value).
                inbound_depth = std::min(inbound_depth, 5);
                if (inbound_depth > 0) {
                    if (auto* trace_ctx = agent_rpc::common::TraceContext::current()) {
                        trace_ctx->setDepth(inbound_depth);
                    }
                }
            }
        }

        if (!isWhitelisted(method_path_)) {
            if (metadata && auth_service_ != nullptr) {
                // P26 T3 dual-mode transition: a valid HMAC-signed identity
                // header set wins without touching PostgreSQL; a present but
                // invalid signature is refused without falling back to the
                // Bearer path (forged headers must not degrade); no
                // signature at all → legacy Bearer validation below.
                if (trust_proxy_enabled_.load(std::memory_order_relaxed)) {
                    const TrustedProxyResult trusted =
                        authenticateTrustedProxy(*metadata);
                    if (trusted == TrustedProxyResult::kAccepted) {
                        tls_auth_.authenticated = true;
                    } else if (trusted == TrustedProxyResult::kRejected) {
                        tls_auth_ = AuthContext{};
                    } else {
                        validateBearerToken(*metadata);
                    }
                } else {
                    validateBearerToken(*metadata);
                }
            }
        } else {
            // Whitelisted methods are always considered authenticated
            tls_auth_.authenticated = true;
        }
    }

    methods->Proceed();
}

bool AuthInterceptor::isWhitelisted(const std::string& method) {
    return method == "/agent_communication.auth.UserService/Register" ||
           method == "/agent_communication.auth.UserService/Login" ||
           method == "/agent_communication.auth.UserService/ValidateToken" ||
           // Restricted public read of a shared conversation. The raw
           // share token is the only credential; the handler stays read-only
           // and sanitized, and expired/revoked shares are refused.
           method == "/agent_communication.SharingService/ReadSharedConversation" ||
           method == "/grpc.health.v1.Health/Check" ||
           method == "/grpc.health.v1.Health/Watch";
}

const AuthInterceptor::AuthContext& AuthInterceptor::currentAuth() {
    return tls_auth_;
}

void AuthInterceptor::propagateAuth(const AuthContext& context) {
    // Worker-thread propagation point: the caller passes a snapshot it
    // copied from its own validated TLS context, so this never invents an
    // identity. Assigning into this thread's TLS makes currentUserId() and
    // isAuthenticated() behave identically to the originating RPC thread.
    tls_auth_ = context;
}

bool AuthInterceptor::isAuthenticated() {
    // Disabled authentication intentionally bypasses enforcement; enabled calls
    // must have an authenticated owner context (unless whitelisted).
    if (!auth_enabled_.load(std::memory_order_relaxed)) return true;
    return tls_auth_.authenticated;
}

std::string AuthInterceptor::currentUserId() {
    return tls_auth_.user_id;
}

std::string AuthInterceptor::currentUsername() {
    return tls_auth_.username;
}

std::string AuthInterceptor::currentRole() {
    return tls_auth_.role;
}

std::string AuthInterceptor::currentTraceId() {
    return tls_auth_.trace_id;
}

std::string AuthInterceptor::currentTokenHash() {
    return tls_auth_.session_token_hash;
}

void AuthInterceptor::validateBearerToken(
    const std::multimap<grpc::string_ref, grpc::string_ref>& metadata) {
    std::string token = extractBearerToken(metadata);
    if (token.empty()) {
        return;
    }
    std::string user_id, username, role;
    std::int64_t expires_at = 0;

    // P26 T2: deny hit → invalid without a PG call; session cache hit with
    // a matching epoch → valid with zero PostgreSQL round-trips; otherwise
    // authoritative PG validation + refill whose TTL equals the session's
    // true remaining lifetime (the fixed 300s bound is gone — revocation is
    // handled by deny/epoch instead).
    const AuthValidationResult outcome =
        AuthCache::validateTokenCached(*auth_service_, token, user_id,
                                       username, role, expires_at);
    if (outcome == AuthValidationResult::kValid && !user_id.empty() &&
        !username.empty()) {
        tls_auth_.authenticated = true;
        tls_auth_.user_id = user_id;
        tls_auth_.username = username;
        tls_auth_.role = role;
    }
}

AuthInterceptor::TrustedProxyResult AuthInterceptor::authenticateTrustedProxy(
    const std::multimap<grpc::string_ref, grpc::string_ref>& metadata) {
    const auto find_value = [&metadata](const char* key) -> std::string {
        auto it = metadata.find(key);
        if (it == metadata.end()) {
            return {};
        }
        return std::string(it->second.data(), it->second.size());
    };

    const std::string signature = find_value("x-nexusai-signature");
    if (signature.empty()) {
        return TrustedProxyResult::kAbsent;
    }
    const std::string user_id = find_value("x-nexusai-user-id");
    const std::string username = find_value("x-nexusai-username");
    const std::string role = find_value("x-nexusai-role");
    const std::string exp_raw = find_value("x-nexusai-exp");
    const std::string token_hash = find_value("x-nexusai-token-hash");
    if (user_id.empty() || username.empty() || role.empty() ||
        exp_raw.empty() || token_hash.empty()) {
        return TrustedProxyResult::kRejected;
    }

    // Replay window: the gateway signs exp = now + 60s on every request, so
    // a stale/expired header is refused here.
    std::int64_t exp = 0;
    try {
        exp = std::stoll(exp_raw);
    } catch (const std::exception&) {
        return TrustedProxyResult::kRejected;
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (now >= exp) {
        return TrustedProxyResult::kRejected;
    }

    // token-hash is part of the signed material: without it an attacker
    // could retarget the Logout revocation at any active session.
    const std::string canonical = user_id + "|" + username + "|" + role +
                                  "|" + exp_raw + "|" + token_hash;
    const std::string expected =
        hmacSha256Hex(proxy_hmac_secret_, canonical);
    if (!constantTimeHexEqual(expected, signature)) {
        return TrustedProxyResult::kRejected;
    }

    tls_auth_.user_id = user_id;
    tls_auth_.username = username;
    tls_auth_.role = role;
    tls_auth_.session_token_hash = token_hash;
    return TrustedProxyResult::kAccepted;
}

grpc::Status AuthInterceptor::requireAdmin() {
    if (!isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Valid authentication token required");
    }
    // Disabled enforcement means a trusted local operator (tests/dev box);
    // enabled calls must carry an ADMIN role resolved from PostgreSQL.
    if (!isAuthEnabled()) {
        return grpc::Status::OK;
    }
    // P26 T3: under trust mode the injected role header is NEVER trusted for
    // admin gates — re-resolve the real role from PostgreSQL (management
    // RPCs are rare; one point query is negligible). Bearer mode roles come
    // from the PG JOIN already, so no re-check is needed there.
    if (trust_proxy_enabled_.load(std::memory_order_relaxed) &&
        auth_service_handle_ != nullptr) {
        const std::string real_role =
            auth_service_handle_->resolveRoleByUserId(tls_auth_.user_id);
        if (real_role != "ADMIN") {
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "Administrator role required");
        }
        return grpc::Status::OK;
    }
    if (tls_auth_.role != "ADMIN") {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "Administrator role required");
    }
    return grpc::Status::OK;
}

void AuthInterceptor::setAuthEnabled(bool enabled) {
    auth_enabled_.store(enabled, std::memory_order_relaxed);
}

bool AuthInterceptor::isAuthEnabled() {
    return auth_enabled_.load(std::memory_order_relaxed);
}

std::string AuthInterceptor::extractBearerToken(
    const std::multimap<grpc::string_ref, grpc::string_ref>& metadata) {
    auto it = metadata.find("authorization");
    if (it == metadata.end()) return {};

    const std::string value(it->second.data(), it->second.size());
    if (!isValidBearerAuthorization(value)) {
        return {};
    }
    return value.substr(sizeof("Bearer ") - 1);
}

}  // namespace server
}  // namespace agent_rpc
