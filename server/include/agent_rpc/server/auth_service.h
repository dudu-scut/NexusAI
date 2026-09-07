#pragma once

#include "agent_rpc/common/auth_repository.h"
#include "user.grpc.pb.h"
#include "user.pb.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace agent_rpc {
namespace server {

// P26 T2: outcome of an authoritative token validation. The interceptor
// collapses kUnavailable into a denial (it has no gRPC response channel);
// the ValidateToken RPC maps it to UNAVAILABLE.
enum class AuthValidationResult { kValid, kInvalid, kUnavailable };

class AuthServiceImpl final : public agent_communication::auth::UserService::Service {
public:
    explicit AuthServiceImpl(common::AuthRepository* repository);
    explicit AuthServiceImpl(common::AuthRepository& repository) : AuthServiceImpl(&repository) {}
    explicit AuthServiceImpl(common::PostgresStore& store);
    ~AuthServiceImpl() override = default;

    grpc::Status Register(
        grpc::ServerContext* context,
        const agent_communication::auth::RegisterRequest* request,
        agent_communication::auth::RegisterResponse* response) override;

    grpc::Status Login(
        grpc::ServerContext* context,
        const agent_communication::auth::LoginRequest* request,
        agent_communication::auth::LoginResponse* response) override;

    grpc::Status ValidateToken(
        grpc::ServerContext* context,
        const agent_communication::auth::ValidateTokenRequest* request,
        agent_communication::auth::ValidateTokenResponse* response) override;

    grpc::Status Logout(
        grpc::ServerContext* context,
        const agent_communication::auth::LogoutRequest* request,
        agent_communication::auth::LogoutResponse* response) override;

    // Status-preserving variant: kUnavailable distinguishes a PostgreSQL
    // outage from a genuinely invalid token (P26 T2; used by AuthCache).
    AuthValidationResult validateTokenWithStatus(
        const std::string& token,
        std::string& user_id,
        std::string& username,
        std::string& role,
        std::int64_t& expires_at);

    // P26 T3: authoritative role lookup by user id — the forced PG re-check
    // behind requireAdmin() under trust mode (the injected role header is
    // never trusted for admin gates). Empty on unknown user / DB failure
    // (fail-closed: callers must refuse).
    std::string resolveRoleByUserId(const std::string& user_id);

    // Public so the shared AuthCache helper (used by the interceptor and the
    // ValidateToken RPC) can derive cache/deny keys from a raw token; the
    // cache lives outside this service, keeping it Redis-free (PostgreSQL
    // remains the sole session fact source).
    static std::string hashToken(const std::string& token);

private:
    static std::string generateId(std::size_t byte_count);
    static std::string generateToken();
    static bool hashPassword(const std::string& password, std::string& encoded_hash);
    static bool verifyPassword(const std::string& password, const std::string& encoded_hash);
    static std::string formatTimestamp(std::chrono::system_clock::time_point time);

    bool validateTokenInternal(const std::string& token,
                               std::string& user_id,
                               std::string& username,
                               std::string& role,
                               std::int64_t& expires_at);

    std::unique_ptr<common::AuthRepository> owned_repository_;
    common::AuthRepository* repository_ = nullptr;  // not owned unless above

    static constexpr int kTokenTtlHours = 24;
    static constexpr int kTokenTtlSeconds = kTokenTtlHours * 3600;
};

}  // namespace server
}  // namespace agent_rpc
