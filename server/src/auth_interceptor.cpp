#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/server/auth_service.h"
#include "agent_rpc/common/redis_client.h"

#include <cstddef>
#include <nlohmann/json.hpp>
#include <chrono>

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

}  // namespace

thread_local AuthInterceptor::AuthContext AuthInterceptor::tls_auth_;
std::atomic<bool> AuthInterceptor::auth_enabled_{false};
common::RedisClient* AuthInterceptor::s_redis_ = nullptr;

void AuthInterceptor::setRedisClient(common::RedisClient* redis) {
    s_redis_ = redis;
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
        }

        if (!isWhitelisted(method_path_)) {
            if (metadata && auth_service_ != nullptr) {
                std::string token = extractBearerToken(*metadata);
                if (!token.empty()) {
                    std::string user_id, username, role;

                    // P22 B: cache-aside read — a hit costs zero PostgreSQL
                    // round-trips. The cache TTL (300s) is a deliberate
                    // trade-off: the interceptor has no channel to the
                    // session expiry timestamp, so a short TTL bounds how
                    // long a revoked/banned session can keep being served.
                    const std::string token_hash =
                        AuthServiceImpl::hashToken(token);
                    const std::string cache_key =
                        "auth:session:" + token_hash;
                    bool cache_hit = false;
                    if (s_redis_ && s_redis_->isConnected()) {
                        std::string cached;
                        if (s_redis_->get(cache_key, cached)) {
                            try {
                                const auto cached_json =
                                    nlohmann::json::parse(cached);
                                user_id = cached_json.value("user_id", "");
                                username = cached_json.value("username", "");
                                role = cached_json.value("role", "");
                                cache_hit = !user_id.empty();
                            } catch (const nlohmann::json::exception&) {
                                cache_hit = false;
                            }
                        }
                    }

                    if (!cache_hit &&
                        auth_service_->validateToken(token, user_id,
                                                     username, role)) {
                        // Cache-aside fill.
                        if (s_redis_ && s_redis_->isConnected() &&
                            !user_id.empty()) {
                            const nlohmann::json payload = {
                                {"user_id", user_id},
                                {"username", username},
                                {"role", role}};
                            s_redis_->setex(cache_key, 300, payload.dump());
                        }
                    }

                    if (!user_id.empty() && !username.empty()) {
                        tls_auth_.authenticated = true;
                        tls_auth_.user_id = user_id;
                        tls_auth_.username = username;
                        tls_auth_.role = role;
                    }
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

grpc::Status AuthInterceptor::requireAdmin() {
    if (!isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Valid authentication token required");
    }
    // Disabled enforcement means a trusted local operator (tests/dev box);
    // enabled calls must carry an ADMIN role resolved from PostgreSQL.
    if (isAuthEnabled() && tls_auth_.role != "ADMIN") {
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
