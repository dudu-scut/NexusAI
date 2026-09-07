#include "agent_rpc/db/migration_runner.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <openssl/sha.h>

#include <pqxx/version>

#if !defined(PQXX_VERSION_MAJOR)
#error "libpqxx must expose PQXX_VERSION_MAJOR"
#endif

namespace agent_rpc::db {
namespace {

constexpr char kMigrationPattern[] = R"(^V([0-9]{3,})__[A-Za-z0-9_]+\.sql$)";

}  // namespace

MigrationRunner::MigrationRunner(common::PostgresStore& store) : store_(store) {}

void MigrationRunner::migrate(const std::filesystem::path& directory) {
    const auto migrations = discover(directory);

    store_.executeTransaction([](pqxx::work& transaction) {
        transaction.exec("SELECT pg_advisory_xact_lock(739214640)");
        transaction.exec(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            "version VARCHAR(64) PRIMARY KEY, "
            "filename TEXT NOT NULL, "
            "checksum CHAR(64) NOT NULL, "
            "applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW())");
    });

    bool applied_any = false;
    for (const auto& migration : migrations) {
        const std::string sql = readFile(migration.path);
        bool applied = false;
        store_.executeTransaction([&](pqxx::work& transaction) {
            transaction.exec("SELECT pg_advisory_xact_lock(739214640)");
#if PQXX_VERSION_MAJOR >= 8
            const pqxx::result result = transaction.exec(
                "SELECT checksum FROM schema_migrations WHERE version = $1",
                pqxx::params{migration.version});
#else  // libpqxx 6.x/7.x
            const pqxx::result result = transaction.exec_params(
                "SELECT checksum FROM schema_migrations WHERE version = $1", migration.version);
#endif
            if (!result.empty()) {
                if (result.front()["checksum"].as<std::string>() != migration.checksum) {
                    throw std::runtime_error("migration checksum mismatch for " + migration.version);
                }
                return;
            }

            transaction.exec(sql);
#if PQXX_VERSION_MAJOR >= 8
            transaction.exec(
                "INSERT INTO schema_migrations(version, filename, checksum) VALUES ($1, $2, $3)",
                pqxx::params{migration.version, migration.path.filename().string(), migration.checksum});
#else  // libpqxx 6.x/7.x
            transaction.exec_params(
                "INSERT INTO schema_migrations(version, filename, checksum) VALUES ($1, $2, $3)",
                migration.version, migration.path.filename().string(), migration.checksum);
#endif
            applied = true;
        });
        applied_any = applied_any || applied;
    }

    std::cout << (applied_any ? "migrations applied" : "migrations already current") << std::endl;
}

std::vector<MigrationRunner::Migration> MigrationRunner::discover(
    const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw std::invalid_argument("migration directory does not exist: " + directory.string());
    }

    const std::regex pattern{kMigrationPattern};
    std::vector<Migration> migrations;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::smatch match;
        const std::string filename = entry.path().filename().string();
        if (!std::regex_match(filename, match, pattern)) {
            // db-R4 (deep-review 2026-09-08): a migration file that does not
            // match V<digits>__name.sql would be silently skipped — but the
            // directory is declared the authoritative schema (AGENTS.md); a
            // typo'd filename must fail loudly instead of quietly never
            // applying. Only non-regular entries (dirs, sockets) skip.
            throw std::invalid_argument(
                "migration file does not match V<version>__name.sql: " + filename);
        }
        migrations.push_back(Migration{
            .version = match[1].str(),
            .path = entry.path(),
            .checksum = checksum(readFile(entry.path())),
        });
    }

    std::sort(migrations.begin(), migrations.end(),
              [](const Migration& left, const Migration& right) {
                  // db-R3 (deep-review 2026-09-08): version strings are
                  // zero-padded V001-V999 today, but the pattern allows 4+
                  // digits — lexicographic order would then put V1000 before
                  // V999. Compare numerically so the execution order always
                  // matches the version numbers.
                  const auto left_version =
                      std::stoull(left.version);
                  const auto right_version =
                      std::stoull(right.version);
                  return left_version < right_version;
              });
    for (std::size_t index = 1; index < migrations.size(); ++index) {
        if (migrations[index - 1].version == migrations[index].version) {
            throw std::invalid_argument("duplicate migration version: " + migrations[index].version);
        }
    }
    return migrations;
}

std::string MigrationRunner::readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot read migration: " + path.string());
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::string MigrationRunner::checksum(const std::string& contents) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(contents.data()), contents.size(), digest.data());
    std::ostringstream encoded;
    for (const unsigned char byte : digest) {
        encoded << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
    }
    return encoded.str();
}

}  // namespace agent_rpc::db
