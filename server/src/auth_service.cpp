#include "agent_rpc/server/auth_service.h"

#include "agent_rpc/common/logger.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace agent_rpc {
namespace server {
namespace {

constexpr std::size_t kTokenBytes = 32;  // 256 bits of entropy.
constexpr std::size_t kTokenHexLength = kTokenBytes * 2;
constexpr std::size_t kSessionIdBytes = 16;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kPasswordHashBytes = 32;
constexpr std::uint64_t kScryptN = 16384;
constexpr std::uint64_t kScryptR = 8;
constexpr std::uint64_t kScryptP = 1;
constexpr std::uint64_t kScryptMaxMemory = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMinUsernameLength = 3;
constexpr std::size_t kMaxUsernameLength = 64;
constexpr std::size_t kMinPasswordLength = 6;
constexpr std::size_t kMaxPasswordLength = 128;

// The administrator identity is configured out-of-band via the
// NEXUSAI_ADMIN_USERNAME environment variable. Default when unset/empty: no
// registration ever receives the ADMIN role. It is read on every call (not
// cached at process start) so operators/tests can adjust it without
// restarting the binary.
std::string configuredAdminUsername() {
    const char* configured = std::getenv("NEXUSAI_ADMIN_USERNAME");
    return configured == nullptr ? std::string{} : std::string{configured};
}

std::string hexEncode(const unsigned char* bytes, std::size_t length) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded(length * 2, '0');
    for (std::size_t index = 0; index < length; ++index) {
        encoded[index * 2] = kHex[(bytes[index] >> 4) & 0x0f];
        encoded[index * 2 + 1] = kHex[bytes[index] & 0x0f];
    }
    return encoded;
}

int hexValue(const char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& encoded, std::vector<unsigned char>& bytes) {
    if (encoded.empty() || encoded.size() % 2 != 0) return false;
    bytes.resize(encoded.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const int high = hexValue(encoded[index * 2]);
        const int low = hexValue(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool validUsername(const std::string& username) {
    if (username.size() < kMinUsernameLength || username.size() > kMaxUsernameLength) {
        return false;
    }
    return std::all_of(username.begin(), username.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '_' || character == '-';
    });
}

bool validPassword(const std::string& password) {
    return password.size() >= kMinPasswordLength && password.size() <= kMaxPasswordLength &&
           password.find('\0') == std::string::npos;
}

template <typename Response>
grpc::Status unavailable(Response* response, const std::string& message) {
    if (response != nullptr) {
        response->mutable_status()->set_code(503);
        response->mutable_status()->set_message(message);
    }
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, message);
}

template <typename Response>
grpc::Status invalidArgument(Response* response, const std::string& message) {
    if (response != nullptr) {
        response->mutable_status()->set_code(400);
        response->mutable_status()->set_message(message);
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, message);
}

template <typename Response>
grpc::Status unauthenticated(Response* response, const std::string& message) {
    if (response != nullptr) {
        response->mutable_status()->set_code(401);
        response->mutable_status()->set_message(message);
    }
    return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, message);
}

std::vector<std::string> splitHash(const std::string& encoded) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= encoded.size()) {
        const std::size_t separator = encoded.find('$', start);
        if (separator == std::string::npos) {
            parts.push_back(encoded.substr(start));
            break;
        }
        parts.push_back(encoded.substr(start, separator - start));
        start = separator + 1;
    }
    return parts;
}

}  // namespace

AuthServiceImpl::AuthServiceImpl(common::AuthRepository* repository)
    : repository_(repository) {}

AuthServiceImpl::AuthServiceImpl(common::PostgresStore& store)
    : owned_repository_(std::make_unique<common::AuthRepository>(store)),
      repository_(owned_repository_.get()) {}

grpc::Status AuthServiceImpl::Register(
    grpc::ServerContext* context,
    const agent_communication::auth::RegisterRequest* request,
    agent_communication::auth::RegisterResponse* response) {
    (void)context;
    if (request == nullptr || response == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required");
    }
    if (!validUsername(request->username()) || !validPassword(request->password())) {
        return invalidArgument(response, "Username must be 3-64 safe characters and password must be 6-128 characters");
    }
    if (request->display_name().size() > 256) {
        return invalidArgument(response, "Display name too long");
    }
    if (repository_ == nullptr) {
        return unavailable(response, "PostgreSQL is unavailable");
    }

    std::string password_hash;
    if (!hashPassword(request->password(), password_hash)) {
        response->mutable_status()->set_code(500);
        response->mutable_status()->set_message("Password hashing unavailable");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Password hashing unavailable");
    }

    const std::string user_id = generateId(kSessionIdBytes);
    if (user_id.empty()) {
        response->mutable_status()->set_code(500);
        response->mutable_status()->set_message("User id generation unavailable");
        return grpc::Status(grpc::StatusCode::INTERNAL, "User id generation unavailable");
    }

    const std::string NEXUSAI_ADMIN_USERNAME = configuredAdminUsername();
    const std::string role =
        !NEXUSAI_ADMIN_USERNAME.empty() && request->username() == NEXUSAI_ADMIN_USERNAME
            ? "ADMIN"
            : "USER";

    const common::UserRecord user{
        .id = user_id,
        .owner_id = user_id,
        .username = request->username(),
        .display_name = request->display_name().empty() ? request->username() : request->display_name(),
        .password_scrypt = password_hash,
        .role = role,
        .created_at = {},
        .updated_at = {},
    };

    try {
        if (!repository_->createUser(user)) {
            response->mutable_status()->set_code(409);
            response->mutable_status()->set_message("Username already exists");
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "Username already exists");
        }
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to persist user: " + std::string(error.what()));
        return unavailable(response, "PostgreSQL is unavailable");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("Registration successful");
    response->set_user_id(user_id);
    response->set_username(request->username());
    response->set_role(role);
    return grpc::Status::OK;
}

grpc::Status AuthServiceImpl::Login(
    grpc::ServerContext* context,
    const agent_communication::auth::LoginRequest* request,
    agent_communication::auth::LoginResponse* response) {
    (void)context;
    if (request == nullptr || response == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required");
    }
    if (!validUsername(request->username()) || !validPassword(request->password())) {
        return invalidArgument(response, "Invalid username or password format");
    }
    if (repository_ == nullptr) {
        return unavailable(response, "PostgreSQL is unavailable");
    }

    std::optional<common::UserRecord> user;
    try {
        user = repository_->findUserByUsername(request->username());
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to read user: " + std::string(error.what()));
        return unavailable(response, "PostgreSQL is unavailable");
    }
    if (!user.has_value() || !verifyPassword(request->password(), user->password_scrypt)) {
        return unauthenticated(response, "Invalid credentials");
    }

    const std::string token = generateToken();
    const std::string token_hash = hashToken(token);
    const std::string session_id = generateId(kSessionIdBytes);
    if (token.empty() || token_hash.empty() || session_id.empty()) {
        response->mutable_status()->set_code(500);
        response->mutable_status()->set_message("Authentication secret generation failed");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Authentication secret generation failed");
    }

    const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours(kTokenTtlHours);
    const auto expires_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        expires_at.time_since_epoch()).count();
    const common::AuthSessionRecord session{
        .id = session_id,
        .owner_id = user->id,
        .token_hash = token_hash,
        .expires_at = formatTimestamp(expires_at),
        .revoked_at = std::nullopt,
        .created_at = {},
        .updated_at = {},
    };

    try {
        if (!repository_->createSession(session)) {
            response->mutable_status()->set_code(500);
            response->mutable_status()->set_message("Failed to persist authentication session");
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist authentication session");
        }
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to persist session: " + std::string(error.what()));
        return unavailable(response, "PostgreSQL is unavailable");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("Login successful");
    response->set_user_id(user->id);
    response->set_username(user->username);
    response->set_token(token);
    response->set_expires_at(expires_timestamp);
    response->set_role(user->role);
    return grpc::Status::OK;
}

grpc::Status AuthServiceImpl::ValidateToken(
    grpc::ServerContext* context,
    const agent_communication::auth::ValidateTokenRequest* request,
    agent_communication::auth::ValidateTokenResponse* response) {
    (void)context;
    if (request == nullptr || response == nullptr) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required");
    }
    if (repository_ == nullptr) {
        return unavailable(response, "PostgreSQL is unavailable");
    }

    std::string user_id;
    std::string username;
    std::string role;
    bool valid = false;
    try {
        valid = validateTokenInternal(request->token(), user_id, username, role);
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to validate token: " + std::string(error.what()));
        return unavailable(response, "PostgreSQL is unavailable");
    }

    response->set_valid(valid);
    if (valid) {
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("Token valid");
        response->set_user_id(user_id);
        response->set_username(username);
        response->set_role(role);
    } else {
        response->mutable_status()->set_code(401);
        response->mutable_status()->set_message("Token invalid or expired");
    }
    return grpc::Status::OK;
}

bool AuthServiceImpl::validateToken(const std::string& token,
                                    std::string& user_id,
                                    std::string& username,
                                    std::string& role) {
    try {
        return validateTokenInternal(token, user_id, username, role);
    } catch (const std::exception& error) {
        LOG_WARN("Token validation unavailable: " + std::string(error.what()));
        user_id.clear();
        username.clear();
        role.clear();
        return false;
    }
}

bool AuthServiceImpl::validateTokenInternal(const std::string& token,
                                            std::string& user_id,
                                            std::string& username,
                                            std::string& role) {
    user_id.clear();
    username.clear();
    role.clear();
    if (repository_ == nullptr || token.size() != kTokenHexLength ||
        !std::all_of(token.begin(), token.end(), [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        })) {
        return false;
    }

    // P22 A: one JOIN query replaces the previous session + user pair of
    // lookups (2 PostgreSQL round-trips → 1).
    const std::string token_hash = hashToken(token);
    const auto session_with_user =
        repository_->findActiveSessionWithUserByTokenHash(token_hash);
    if (!session_with_user.has_value()) {
        return false;
    }
    user_id = session_with_user->user_id;
    username = session_with_user->username;
    role = session_with_user->role;
    return true;
}

std::string AuthServiceImpl::generateId(const std::size_t byte_count) {
    if (byte_count == 0) return {};
    std::vector<unsigned char> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return {};
    }
    return hexEncode(bytes.data(), bytes.size());
}

std::string AuthServiceImpl::generateToken() {
    std::array<unsigned char, 32> random_bytes{};
    if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
        return {};
    }
    return hexEncode(random_bytes.data(), random_bytes.size());
}

std::string AuthServiceImpl::hashToken(const std::string& token) {
    if (token.empty()) return {};
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest.data()) == nullptr) {
        return {};
    }
    return hexEncode(digest.data(), digest.size());
}

bool AuthServiceImpl::hashPassword(const std::string& password, std::string& encoded_hash) {
    std::array<unsigned char, kSaltBytes> salt{};
    std::array<unsigned char, kPasswordHashBytes> derived{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(),
                       kScryptN, kScryptR, kScryptP, kScryptMaxMemory,
                       derived.data(), derived.size()) != 1) {
        encoded_hash.clear();
        return false;
    }

    encoded_hash = "scrypt$" + std::to_string(kScryptN) + "$" + std::to_string(kScryptR) + "$" +
                   std::to_string(kScryptP) + "$" + hexEncode(salt.data(), salt.size()) + "$" +
                   hexEncode(derived.data(), derived.size());
    return true;
}

bool AuthServiceImpl::verifyPassword(const std::string& password,
                                     const std::string& encoded_hash) {
    const auto parts = splitHash(encoded_hash);
    if (parts.size() != 6 || parts[0] != "scrypt") return false;

    std::uint64_t n = 0;
    std::uint64_t r = 0;
    std::uint64_t p = 0;
    try {
        n = std::stoull(parts[1]);
        r = std::stoull(parts[2]);
        p = std::stoull(parts[3]);
    } catch (const std::exception&) {
        return false;
    }
    if (n != kScryptN || r != kScryptR || p != kScryptP) return false;

    std::vector<unsigned char> salt;
    std::vector<unsigned char> expected;
    if (!hexDecode(parts[4], salt) || !hexDecode(parts[5], expected) ||
        salt.size() != kSaltBytes || expected.size() != kPasswordHashBytes) {
        return false;
    }

    std::array<unsigned char, kPasswordHashBytes> derived{};
    if (EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(),
                       n, r, p, kScryptMaxMemory, derived.data(), derived.size()) != 1) {
        return false;
    }
    return CRYPTO_memcmp(derived.data(), expected.data(), expected.size()) == 0;
}

std::string AuthServiceImpl::formatTimestamp(const std::chrono::system_clock::time_point time) {
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &timestamp) != 0) return {};
#else
    if (gmtime_r(&timestamp, &utc) == nullptr) return {};
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M:%S+00");
    return output.str();
}

}  // namespace server
}  // namespace agent_rpc
