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
        // P26 T3: session token hash carried by the trusted proxy's injected
        // header (Bearer is consumed at the gateway). Logout revokes through
        // this value; empty in the legacy Bearer path.
        std::string session_token_hash;
    };

    static const AuthContext& currentAuth();
    static bool isAuthenticated();
    static std::string currentUserId();
    static std::string currentUsername();
    static std::string currentRole();
    static std::string currentTraceId();

    // P26 T3: token hash resolved by the interceptor (trusted-proxy headers)
    // — the revocation handle for Logout when the gateway consumed the Bearer.
    static std::string currentTokenHash();

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

    // Public so AuthServiceImpl::Logout can parse the very same authorization
    // metadata the interceptor validated (one parsing implementation, no
    // drift between the gate and the revocation handler).
    static std::string extractBearerToken(
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata);

    // P26 T2: session cache accessor injected by RpcServer. Forwards to the
    // shared AuthCache helper (cache read-modify-write lives there so the
    // auth interceptor and the ValidateToken RPC share one code path) while
    // AuthServiceImpl stays Redis-free (contract-locked). Null/disconnected
    // Redis degrades to the authoritative PG JOIN lookup.
    static void setRedisClient(common::RedisClient* redis);

    // P26 T3 (trusted proxy, phase 1+2): configures HMAC verification of the
    // injected x-nexusai-* headers. RpcServer calls this at assembly from
    // NEXUSAI_TRUST_PROXY / NEXUSAI_PROXY_HMAC_SECRET; an empty secret while
    // trust=1 must fail startup (fail-fast), never degrade silently.
    static void setTrustedProxy(const std::string& hmac_secret, bool enabled);
    static bool isTrustProxyEnabled();

    // P26 T3: auth service handle for requireAdmin()'s forced PG re-check of
    // the real role under trust mode (the injected role is never trusted for
    // admin gates). Set once by RpcServer at assembly; not owned.
    static void setAuthService(AuthServiceImpl* auth_service);

private:
    enum class TrustedProxyResult { kAbsent, kAccepted, kRejected };

    // P26 T3: verifies x-nexusai-signature + exp against the shared secret
    // and fills tls_auth_ on success. kAbsent = no signature header (fall
    // through to the legacy Bearer path); kRejected = signature present but
    // invalid/expired — refuse without falling back (forged headers must not
    // degrade into Bearer validation).
    static TrustedProxyResult authenticateTrustedProxy(
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata);

    // P26 T3: legacy Bearer validation (AuthCache path) factored out so the
    // dual-mode gate can fall through to it when no signature header exists.
    void validateBearerToken(
        const std::multimap<grpc::string_ref, grpc::string_ref>& metadata);

    AuthServiceImpl* auth_service_;
    std::string method_path_;

    static thread_local AuthContext tls_auth_;
    static std::atomic<bool> auth_enabled_;
    static std::atomic<bool> trust_proxy_enabled_;
    static std::string proxy_hmac_secret_;
    static AuthServiceImpl* auth_service_handle_;
};

}  // namespace server
}  // namespace agent_rpc
