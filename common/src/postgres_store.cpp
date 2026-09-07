#include "agent_rpc/common/postgres_store.h"

#include <charconv>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace agent_rpc::common {
namespace {

std::string Required(const PostgresConfig::EnvironmentLookup& lookup, const std::string& name) {
    const std::string value = lookup(name);
    if (value.empty()) {
        throw std::invalid_argument(name + " must be set");
    }
    return value;
}

int ParsePort(const std::string& value) {
    if (value.empty()) {
        throw std::invalid_argument("NEXUSAI_POSTGRES_PORT must be between 1 and 65535");
    }
    for (const unsigned char character : value) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument("NEXUSAI_POSTGRES_PORT must be a decimal number");
        }
    }

    int port = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || port < 1 || port > 65535) {
        throw std::invalid_argument("NEXUSAI_POSTGRES_PORT must be between 1 and 65535");
    }
    return port;
}

int ParsePoolSize(const std::string& value) {
    if (value.empty()) {
        throw std::invalid_argument("NEXUSAI_POSTGRES_POOL_SIZE must be between 1 and 10");
    }
    for (const unsigned char character : value) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument("NEXUSAI_POSTGRES_POOL_SIZE must be a decimal number");
        }
    }

    int pool_size = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), pool_size);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || pool_size < 1 ||
        pool_size > PostgresConfig::kMaxPoolSize) {
        throw std::invalid_argument("NEXUSAI_POSTGRES_POOL_SIZE must be between 1 and 10");
    }
    return pool_size;
}

void RejectNul(const std::string& value, const std::string& name) {
    if (value.find('\0') != std::string::npos) {
        throw std::invalid_argument(name + " must not contain NUL");
    }
}

std::string EscapeConnectionValue(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '\'') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

}  // namespace

PostgresConfig PostgresConfig::fromEnvironment() {
    return fromEnvironment([](const std::string& name) {
        const char* value = std::getenv(name.c_str());
        return value == nullptr ? std::string{} : std::string{value};
    });
}

PostgresConfig PostgresConfig::fromEnvironment(const EnvironmentLookup& lookup) {
    PostgresConfig config{
        .host = Required(lookup, "NEXUSAI_POSTGRES_HOST"),
        .database = Required(lookup, "NEXUSAI_POSTGRES_DATABASE"),
        .user = Required(lookup, "NEXUSAI_POSTGRES_USER"),
        .password = Required(lookup, "NEXUSAI_POSTGRES_PASSWORD"),
    };

    RejectNul(config.host, "NEXUSAI_POSTGRES_HOST");
    RejectNul(config.database, "NEXUSAI_POSTGRES_DATABASE");
    RejectNul(config.user, "NEXUSAI_POSTGRES_USER");
    RejectNul(config.password, "NEXUSAI_POSTGRES_PASSWORD");

    const std::string port = lookup("NEXUSAI_POSTGRES_PORT");
    config.port = port.empty() ? 5432 : ParsePort(port);

    const std::string pool_size = lookup("NEXUSAI_POSTGRES_POOL_SIZE");
    config.pool_size = pool_size.empty() ? PostgresConfig::kDefaultPoolSize : ParsePoolSize(pool_size);
    return config;
}

namespace detail {

PostgresPoolLeaseCoordinator::PostgresPoolLeaseCoordinator(const int pool_size) {
    if (pool_size < 1 || pool_size > PostgresConfig::kMaxPoolSize) {
        throw std::invalid_argument("NEXUSAI_POSTGRES_POOL_SIZE must be between 1 and 10");
    }
    leased_.assign(static_cast<std::size_t>(pool_size), false);
}

std::size_t PostgresPoolLeaseCoordinator::acquire() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        for (const bool leased : leased_) {
            if (!leased) {
                return true;
            }
        }
        return false;
    });

    for (std::size_t index = 0; index < leased_.size(); ++index) {
        if (!leased_[index]) {
            leased_[index] = true;
            return index;
        }
    }

    throw std::logic_error("PostgresPoolLeaseCoordinator lost an available lease");
}

void PostgresPoolLeaseCoordinator::release(const std::size_t index) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (index < leased_.size()) {
            leased_[index] = false;
        }
    }
    condition_.notify_one();
}

std::size_t PostgresPoolLeaseCoordinator::size() const noexcept {
    return leased_.size();
}

}  // namespace detail

PostgresStore::PostgresStore(PostgresConfig config)
    : config_(std::move(config)), lease_coordinator_(config_.pool_size) {
    try {
        connections_.reserve(lease_coordinator_.size());
        for (std::size_t index = 0; index < lease_coordinator_.size(); ++index) {
            connections_.push_back(createConnection());
        }
    } catch (const PostgresUnavailable&) {
        connections_.clear();
        throw;
    } catch (const std::exception& error) {
        connections_.clear();
        throw PostgresUnavailable(std::string{"PostgreSQL is unavailable: "} + error.what());
    }
}

PostgresStore::~PostgresStore() = default;

void PostgresStore::executeTransaction(const std::function<void(pqxx::work&)>& operation) {
    if (!operation) {
        throw std::invalid_argument("PostgresStore transaction callback must not be empty");
    }

    const std::size_t index = lease_coordinator_.acquire();
    try {
        pqxx::connection& connection = ensureConnection(index);
        {
            pqxx::work transaction{connection};
            operation(transaction);
            transaction.commit();
        }
        lease_coordinator_.release(index);
    } catch (const pqxx::sql_error&) {
        // PR-C3 defense: concrete SQL errors (e.g. pqxx::unique_violation,
        // carrying the SQLSTATE callers depend on) must always escape with
        // their exact type. Catch and rethrow before any broader pqxx
        // handler so a future libpqxx 8.x inheritance change can never let a
        // wider catch swallow them.
        lease_coordinator_.release(index);
        throw;
    } catch (const pqxx::broken_connection& error) {
        connections_[index].reset();
        lease_coordinator_.release(index);
        throw PostgresUnavailable(std::string{"PostgreSQL is unavailable: "} + error.what());
    } catch (const pqxx::failure& error) {
        // PR-C3 defense: remaining pqxx failures (non-SQL) map to the
        // platform's unavailable contract, still after the sql_error guard.
        connections_[index].reset();
        lease_coordinator_.release(index);
        throw PostgresUnavailable(std::string{"PostgreSQL is unavailable: "} + error.what());
    } catch (...) {
        lease_coordinator_.release(index);
        throw;
    }
}

bool PostgresStore::healthCheck() noexcept {
    try {
        executeTransaction([](pqxx::work& transaction) { transaction.exec("SELECT 1"); });
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::unique_ptr<pqxx::connection> PostgresStore::createConnection() const {
    try {
        auto connection = std::make_unique<pqxx::connection>(connectionString());
        if (!connection->is_open()) {
            throw PostgresUnavailable("PostgreSQL is unavailable: connection did not open");
        }
        return connection;
    } catch (const PostgresUnavailable&) {
        throw;
    } catch (const std::exception& error) {
        throw PostgresUnavailable(std::string{"PostgreSQL is unavailable: "} + error.what());
    }
}

pqxx::connection& PostgresStore::ensureConnection(const std::size_t index) {
    if (index >= connections_.size()) {
        throw std::logic_error("PostgresStore connection lease is out of range");
    }

    auto& connection = connections_[index];
    if (!connection || !connection->is_open()) {
        connection = createConnection();
    }
    return *connection;
}

std::string PostgresStore::connectionString() const {
    std::ostringstream stream;
    stream << "host='" << EscapeConnectionValue(config_.host) << "' "
           << "port='" << config_.port << "' "
           << "dbname='" << EscapeConnectionValue(config_.database) << "' "
           << "user='" << EscapeConnectionValue(config_.user) << "' "
           << "password='" << EscapeConnectionValue(config_.password) << "'";
    // deep-review lc-R3: bound the TCP connect so a network black-hole (not
    // a refusal) cannot wedge initialize()/health checks/stop() forever —
    // libpq defaults to an unbounded connect attempt. Keepalives detect
    // half-dead peers on long-lived pooled connections.
    stream << " connect_timeout=5 keepalives=1 keepalives_idle=30";
    return stream.str();
}

}  // namespace agent_rpc::common
