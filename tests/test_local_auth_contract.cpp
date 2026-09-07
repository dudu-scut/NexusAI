#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>

#ifndef NEXUSAI_AUTH_SERVICE_HEADER
#error "NEXUSAI_AUTH_SERVICE_HEADER must point at auth_service.h"
#endif

#ifndef NEXUSAI_AUTH_SERVICE_SOURCE
#error "NEXUSAI_AUTH_SERVICE_SOURCE must point at auth_service.cpp"
#endif

#ifndef NEXUSAI_RPC_SERVER_HEADER
#error "NEXUSAI_RPC_SERVER_HEADER must point at rpc_server.h"
#endif

#ifndef NEXUSAI_RPC_SERVER_SOURCE
#error "NEXUSAI_RPC_SERVER_SOURCE must point at rpc_server.cpp"
#endif

#ifndef NEXUSAI_AUTH_REPOSITORY_HEADER
#error "NEXUSAI_AUTH_REPOSITORY_HEADER must point at auth_repository.h"
#endif

namespace {

std::string readFile(const char* path) {
    std::ifstream source{path, std::ios::binary};
    EXPECT_TRUE(source.is_open()) << "cannot read " << path;
    return {std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
}

void expectContains(const std::string& source, const std::string& fragment) {
    EXPECT_NE(source.find(fragment), std::string::npos) << "missing: " << fragment;
}

void expectNotContains(const std::string& source, const std::string& fragment) {
    EXPECT_EQ(source.find(fragment), std::string::npos) << "forbidden: " << fragment;
}

TEST(LocalAuthContractTest, AuthServiceUsesPostgresAndConstantTimeCrypto) {
    const std::string header = readFile(NEXUSAI_AUTH_SERVICE_HEADER);
    const std::string source = readFile(NEXUSAI_AUTH_SERVICE_SOURCE);
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    expectContains(header, "auth_repository.h");
    expectContains(source, "EVP_PBE_scrypt");
    expectContains(source, "CRYPTO_memcmp");
    expectContains(source, "RAND_bytes");
    expectContains(source, "SHA256");
    expectContains(source, "token_hash");
    expectContains(source, ".token_hash = token_hash");
    expectContains(header, "kTokenTtlHours = 24");
    expectContains(source, "kTokenTtlHours");
    expectContains(source, "NEXUSAI_ADMIN_USERNAME");
    expectContains(source, "getenv(\"NEXUSAI_ADMIN_USERNAME\")");
    expectContains(source, "!NEXUSAI_ADMIN_USERNAME.empty()");

    for (const std::string forbidden : {"RedisClient", "redis_", "nexusai:user:",
                                        "nexusai:token:", "nexusai:uid:"}) {
        expectNotContains(header, forbidden);
        expectNotContains(source, forbidden);
    }
}

TEST(LocalAuthContractTest, TokenIsOpaqueRandom256BitValueAndOnlyItsHashIsPersisted) {
    const std::string source = readFile(NEXUSAI_AUTH_SERVICE_SOURCE);
    ASSERT_FALSE(source.empty());

    expectContains(source, "std::array<unsigned char, 32>");
    expectContains(source, "RAND_bytes");
    expectContains(source, "SHA256");
    expectContains(source, "createSession");
    expectContains(source, "token_hash");
    expectNotContains(source, ".token_hash = token,");
    expectNotContains(source, "generateUuid()");
}

TEST(LocalAuthContractTest, RepositorySupportsUserIdLookupAndRpcServerOwnsPostgresStore) {
    const std::string repository_header = readFile(NEXUSAI_AUTH_REPOSITORY_HEADER);
    const std::string rpc_header = readFile(NEXUSAI_RPC_SERVER_HEADER);
    const std::string rpc_source = readFile(NEXUSAI_RPC_SERVER_SOURCE);
    ASSERT_FALSE(repository_header.empty());
    ASSERT_FALSE(rpc_header.empty());
    ASSERT_FALSE(rpc_source.empty());

    expectContains(repository_header, "findUserById");
    expectContains(rpc_header, "PostgresStore");
    expectContains(rpc_source, "PostgresConfig::fromEnvironment");
    expectContains(rpc_source, "AuthServiceImpl");
    expectContains(rpc_source, "PostgresUnavailable");
    // P26 T3: trust mode must be assembled with a fail-fast secret check
    // (never silently trust unsigned headers) and admin re-checks must be
    // able to reach the auth service.
    expectContains(rpc_source, "NEXUSAI_TRUST_PROXY");
    expectContains(rpc_source, "NEXUSAI_PROXY_HMAC_SECRET");
    expectContains(rpc_source, "setTrustedProxy");
    expectContains(rpc_source, "setAuthService");
}

}  // namespace
