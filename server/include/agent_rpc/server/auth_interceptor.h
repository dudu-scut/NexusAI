#pragma once

#include <grpcpp/grpcpp.h>
#include <atomic>
#include <string>

namespace agent_rpc {
namespace common { class RedisClient; }
namespace server {

class AuthServiceImpl;

/**
 * @brief gRPC server interceptor for token-based authentication.
 *
 * Extracts and validates the "authorization" metadata from incoming RPCs.
 * Stores user identity in thread-local storage for downstream handlers.
 * Whitelisted methods (UserService.Register/Login/ValidateToken and gRPC health
 * checks) bypass authentication.
 *
 * Handlers call currentUserId(), currentUsername(), currentTraceId(), and
 * isAuthenticated() to inspect the request owner context.
 */
class AuthInterceptor : public grpc::experimental::Interceptor {
public:
    AuthInterceptor(AuthServiceImpl* auth_service,
                    const std::string& method_path);

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

    // Check if a method path is exempt from authentication
    static bool isWhitelisted(const std::string& method);

    // Thread-local auth state (set by interceptor, read by handlers)

    struct AuthContext {
        std::string user_id;
        std::string username;
        std::string role;
        bool authenticated = false;
        std::string trace_id;
    };

    static const AuthContext& currentAuth();
    static bool isAuthenticated();
    static std::string currentUserId();
    static std::string currentUsername();
    static std::string currentRole();
    static std::string currentTraceId();

    // Copies an already-validated auth context onto the CALLING
    // thread's TLS. Server-spawned worker threads (e.g. the parallel compare
    // executors) never see the interceptor, so the serving thread propagates
    // its own currentAuth() snapshot before spawning them. The context is
    // never fabricated here — only a context the interceptor already
    // validated can be propagated.
    static void propagateAuth(const AuthContext& context);

    // Server-side admin gate for management RPCs. Returns UNAUTHENTICATED when
    // no valid owner context exists and PERMISSION_DENIED when the caller is
    // not ADMIN. When auth enforcement is disabled the call is treated as an
    // authorized local operator (mirrors isAuthenticated()).
    static grpc::Status requireAdmin();

    // Auth enable flag: when false, isAuthenticated() returns true (no enforcement)
    static void setAuthEnabled(bool enabled);
    static bool isAuthEnabled();

    // P22 B: cache-aside session cache injected by RpcServer. The cache lives
    // HERE (the consumer layer) so AuthServiceImpl stays Redis-free and
    // PostgreSQL remains the sole session fact source (contract-locked).
    // Null/disconnected Redis degrades to the authoritative PG JOIN lookup.
    static void setRedisClient(common::RedisClient* redis);

private:
    static std::string extractBearerToken(
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata);

    AuthServiceImpl* auth_service_;
    std::string method_path_;

    static common::RedisClient* s_redis_;
    static thread_local AuthContext tls_auth_;
    static std::atomic<bool> auth_enabled_;
};

}  // namespace server
}  // namespace agent_rpc
