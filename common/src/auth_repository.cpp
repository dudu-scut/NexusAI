#include "agent_rpc/common/auth_repository.h"

#include <pqxx/version>

#include <string>
#include <utility>

namespace agent_rpc::common {
namespace {

template <typename... Arguments>
pqxx::result execParams(pqxx::work& transaction, const std::string& query, Arguments&&... arguments) {
#if PQXX_VERSION_MAJOR >= 8
    return transaction.exec(query, pqxx::params{std::forward<Arguments>(arguments)...});
#else  // libpqxx 6.x/7.x
    return transaction.exec_params(query, std::forward<Arguments>(arguments)...);
#endif
}

template <typename Row>
UserRecord userFromRow(const Row& row) {
    return UserRecord{
        .id = row["id"].template as<std::string>(),
        .owner_id = row["owner_id"].template as<std::string>(),
        .username = row["username"].template as<std::string>(),
        .display_name = row["display_name"].template as<std::string>(),
        .password_scrypt = row["password_scrypt"].template as<std::string>(),
        .role = row["role"].template as<std::string>(),
        .created_at = row["created_at"].template as<std::string>(),
        .updated_at = row["updated_at"].template as<std::string>(),
    };
}

template <typename Row>
AuthSessionRecord sessionFromRow(const Row& row) {
    AuthSessionRecord session{
        .id = row["id"].template as<std::string>(),
        .owner_id = row["owner_id"].template as<std::string>(),
        .token_hash = row["token_hash"].template as<std::string>(),
        .expires_at = row["expires_at"].template as<std::string>(),
        .revoked_at = std::nullopt,
        .created_at = row["created_at"].template as<std::string>(),
        .updated_at = row["updated_at"].template as<std::string>(),
    };
    if (!row["revoked_at"].is_null()) {
        session.revoked_at = row["revoked_at"].template as<std::string>();
    }
    return session;
}

}  // namespace

AuthRepository::AuthRepository(PostgresStore& store) : store_(store) {}

bool AuthRepository::createUser(const UserRecord& user) {
    bool inserted = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO users (id, owner_id, username, display_name, password_scrypt, role, created_at, updated_at) "
            "VALUES ($1, $2, $3, $4, $5, $6, "
            "COALESCE(NULLIF($7, '')::timestamptz, NOW()), "
            "COALESCE(NULLIF($8, '')::timestamptz, NOW())) "
            "ON CONFLICT DO NOTHING RETURNING id",
            user.id, user.owner_id, user.username, user.display_name, user.password_scrypt, user.role,
            user.created_at, user.updated_at);
        inserted = !result.empty();
    });
    return inserted;
}

std::optional<UserRecord> AuthRepository::findUserByUsername(const std::string& username) {
    std::optional<UserRecord> user;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, username, display_name, password_scrypt, role, "
            "created_at::text AS created_at, updated_at::text AS updated_at "
            "FROM users WHERE username = $1",
            username);
        if (!result.empty()) {
            user = userFromRow(result.front());
        }
    });
    return user;
}

std::optional<UserRecord> AuthRepository::findUserById(const std::string& user_id) {
    std::optional<UserRecord> user;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, username, display_name, password_scrypt, role, "
            "created_at::text AS created_at, updated_at::text AS updated_at "
            "FROM users WHERE id = $1",
            user_id);
        if (!result.empty()) {
            user = userFromRow(result.front());
        }
    });
    return user;
}
bool AuthRepository::createSession(const AuthSessionRecord& session) {
    bool inserted = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO auth_sessions "
            "(id, owner_id, token_hash, expires_at, revoked_at, created_at, updated_at) "
            "VALUES ($1, $2, $3, $4::timestamptz, NULLIF($5, '')::timestamptz, "
            "COALESCE(NULLIF($6, '')::timestamptz, NOW()), "
            "COALESCE(NULLIF($7, '')::timestamptz, NOW())) "
            "ON CONFLICT DO NOTHING RETURNING id",
            session.id, session.owner_id, session.token_hash, session.expires_at,
            session.revoked_at.value_or(std::string{}), session.created_at, session.updated_at);
        inserted = !result.empty();
    });
    return inserted;
}

std::optional<AuthSessionRecord> AuthRepository::findActiveSessionByTokenHash(
    const std::string& token_hash) {
    std::optional<AuthSessionRecord> session;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, token_hash, expires_at::text AS expires_at, "
            "revoked_at::text AS revoked_at, created_at::text AS created_at, "
            "updated_at::text AS updated_at "
            "FROM auth_sessions "
            "WHERE token_hash = $1 AND revoked_at IS NULL AND expires_at > NOW()",
            token_hash);
        if (!result.empty()) {
            session = sessionFromRow(result.front());
        }
    });
    return session;
}

std::optional<AuthSessionWithUser> AuthRepository::findActiveSessionWithUserByTokenHash(
    const std::string& token_hash) {
    // P22 A: single round-trip replaces the previous session + user pair of
    // lookups. password_scrypt is not selected on purpose.
    std::optional<AuthSessionWithUser> session_with_user;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT s.id, s.owner_id, s.token_hash, s.expires_at::text AS expires_at, "
            "s.revoked_at::text AS revoked_at, s.created_at::text AS created_at, "
            "s.updated_at::text AS updated_at, u.id AS user_id, u.username, u.role, "
            "EXTRACT(EPOCH FROM s.expires_at)::bigint AS expires_epoch "
            "FROM auth_sessions s JOIN users u ON u.id = s.owner_id "
            "WHERE s.token_hash = $1 AND s.revoked_at IS NULL "
            "AND s.expires_at > NOW()",
            token_hash);
        if (!result.empty()) {
            const auto& row = result.front();
            AuthSessionWithUser joined;
            joined.session = sessionFromRow(row);
            joined.user_id = row["user_id"].is_null()
                                 ? std::string{}
                                 : row["user_id"].template as<std::string>();
            joined.username = row["username"].is_null()
                                  ? std::string{}
                                  : row["username"].template as<std::string>();
            joined.role = row["role"].is_null()
                              ? std::string{}
                              : row["role"].template as<std::string>();
            joined.expires_epoch =
                row["expires_epoch"].is_null()
                    ? 0
                    : row["expires_epoch"].template as<std::int64_t>();
            session_with_user = std::move(joined);
        }
    });
    return session_with_user;
}

bool AuthRepository::revokeSession(const std::string& session_id) {
    bool revoked = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "UPDATE auth_sessions SET revoked_at = NOW(), updated_at = NOW() "
            "WHERE id = $1 AND revoked_at IS NULL RETURNING id",
            session_id);
        revoked = !result.empty();
    });
    return revoked;
}

}  // namespace agent_rpc::common
