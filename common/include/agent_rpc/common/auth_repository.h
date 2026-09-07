#pragma once

#include "agent_rpc/common/postgres_store.h"

#include <cstdint>
#include <optional>
#include <string>

namespace agent_rpc::common {

struct UserRecord {
    std::string id;
    std::string owner_id;
    std::string username;
    std::string display_name;
    std::string password_scrypt;
    std::string role;
    std::string created_at;
    std::string updated_at;
};

struct AuthSessionRecord {
    std::string id;
    std::string owner_id;
    std::string token_hash;
    std::string expires_at;
    std::optional<std::string> revoked_at;
    std::string created_at;
    std::string updated_at;
};

// P22 A: one-query JOIN of the active session with its owning user, so
// token validation needs a single PostgreSQL round-trip instead of two
// (session lookup + user lookup). password_scrypt is deliberately not
// selected — session validation never needs the password hash.
// expires_epoch (Unix seconds, from EXTRACT(EPOCH)) gives the caller a
// numeric expiry without parsing the timestamptz text (P26 T2: the auth
// cache TTL is the session's true remaining lifetime, not a fixed bound).
struct AuthSessionWithUser {
    AuthSessionRecord session;
    std::string user_id;
    std::string username;
    std::string role;
    std::int64_t expires_epoch = 0;
};

class AuthRepository final {
public:
    explicit AuthRepository(PostgresStore& store);

    bool createUser(const UserRecord& user);
    std::optional<UserRecord> findUserByUsername(const std::string& username);
    std::optional<UserRecord> findUserById(const std::string& user_id);
    bool createSession(const AuthSessionRecord& session);
    std::optional<AuthSessionRecord> findActiveSessionByTokenHash(const std::string& token_hash);
    std::optional<AuthSessionWithUser> findActiveSessionWithUserByTokenHash(
        const std::string& token_hash);
    bool revokeSession(const std::string& session_id);

private:
    PostgresStore& store_;
};

}  // namespace agent_rpc::common
