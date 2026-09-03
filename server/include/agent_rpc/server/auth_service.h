#pragma once

#include "agent_rpc/common/auth_repository.h"
#include "user.grpc.pb.h"
#include "user.pb.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace agent_rpc {
namespace server {

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

    // Internal validation used by the authentication interceptor. Database
    // errors are deliberately collapsed to false here because the interceptor
    // has no gRPC response in which to return UNAVAILABLE. The resolved role
    // (USER/ADMIN) comes from PostgreSQL and is propagated to the interceptor.
    bool validateToken(const std::string& token,
                       std::string& user_id,
                       std::string& username,
                       std::string& role);

    // Public so AuthInterceptor can derive the cache-aside key (P22 B); the
    // cache lives in the interceptor layer, keeping this service Redis-free
    // (PostgreSQL remains the sole session fact source).
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
                               std::string& role);

    std::unique_ptr<common::AuthRepository> owned_repository_;
    common::AuthRepository* repository_ = nullptr;  // not owned unless above

    static constexpr int kTokenTtlHours = 24;
    static constexpr int kTokenTtlSeconds = kTokenTtlHours * 3600;
};

}  // namespace server
}  // namespace agent_rpc
