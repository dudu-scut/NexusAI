#include "agent_rpc/common/postgres_budget_repository.h"

#include <pqxx/version>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
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

void rejectNul(const std::string& value, const char* name) {
    if (value.find('\0') != std::string::npos) {
        throw std::invalid_argument(std::string{name} + " must not contain NUL");
    }
}

void validateId(const std::string& value, const char* name) {
    if (value.empty()) {
        throw std::invalid_argument(std::string{name} + " must not be empty");
    }
    rejectNul(value, name);
}

void validateLimits(const BudgetLimits& limits) {
    if (limits.global < 0 || limits.user_daily < 0 || limits.user_monthly < 0 || limits.session < 0) {
        throw std::invalid_argument("budget limits must be non-negative (zero means unlimited)");
    }
}

struct Bucket {
    std::string type;
    std::string owner_id;
    std::string session_id;
    std::string bucket_start;
    const char* reason = "budget exceeded";
};

std::array<Bucket, 4> bucketsFor(const std::string& owner_id, const std::string& context_id,
                                 const std::string& day, const std::string& month) {
    return {
        Bucket{"global", "__global__", "__global__", "2000-01-01", "global budget exceeded"},
        Bucket{"user_daily", owner_id, "__owner__", day, "daily budget exceeded"},
        Bucket{"user_monthly", owner_id, "__owner__", month, "monthly budget exceeded"},
        Bucket{"session", owner_id, context_id, "2000-01-01", "session budget exceeded"},
    };
}

void lockBucket(pqxx::work& transaction, const Bucket& bucket) {
    const std::string lock_key = bucket.type + "|" + bucket.owner_id + "|" + bucket.session_id + "|" +
                                 bucket.bucket_start;
    // Advisory locks cover the absent-row case.  Without them two concurrent
    // first reservations could both observe a missing counter and overspend.
    (void)execParams(transaction, "SELECT pg_advisory_xact_lock(hashtext($1)::bigint)", lock_key);
}

std::int64_t readCounter(pqxx::work& transaction, const Bucket& bucket, const bool lock_row) {
    std::string query =
        "SELECT used_tokens FROM budget_counters "
        "WHERE bucket_type = $1 AND owner_id = $2 AND session_id = $3 AND bucket_start = $4::date";
    if (lock_row) {
        query += " FOR UPDATE";
    }
    const auto result = execParams(transaction, query, bucket.type, bucket.owner_id, bucket.session_id,
                                   bucket.bucket_start);
    if (result.empty()) {
        return 0;
    }
    return result.front()["used_tokens"].template as<std::int64_t>();
}

void addCounter(pqxx::work& transaction, const Bucket& bucket, const std::int64_t estimated_tokens) {
    (void)execParams(
        transaction,
        "INSERT INTO budget_counters "
        "(bucket_type, owner_id, session_id, bucket_start, used_tokens, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4::date, $5, NOW(), NOW()) "
        "ON CONFLICT (bucket_type, owner_id, session_id, bucket_start) DO UPDATE SET "
        "used_tokens = budget_counters.used_tokens + EXCLUDED.used_tokens, updated_at = NOW()",
        bucket.type, bucket.owner_id, bucket.session_id, bucket.bucket_start, estimated_tokens);
}

bool wouldExceed(const std::int64_t used, const std::int64_t limit, const std::int64_t requested) {
    if (requested > std::numeric_limits<std::int64_t>::max() - used) {
        return true;
    }
    if (limit == 0) {
        return false;
    }
    return used > limit || requested > limit - used;
}

}  // namespace

PostgresBudgetRepository::PostgresBudgetRepository(PostgresStore& store) : store_(store) {}

BudgetReservationResult PostgresBudgetRepository::reserve(const std::string& owner_id,
                                                          const std::string& context_id,
                                                          const std::string& request_id,
                                                          const std::int64_t estimated_tokens,
                                                          const BudgetLimits& limits) {
    validateId(owner_id, "owner_id");
    validateId(context_id, "context_id");
    validateId(request_id, "request_id");
    if (estimated_tokens < 0) {
        throw std::invalid_argument("estimated_tokens must be non-negative");
    }
    validateLimits(limits);

    BudgetReservationResult result;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto date_result = transaction.exec(
            "SELECT CURRENT_DATE::text AS day, "
            "date_trunc('month', CURRENT_DATE)::date::text AS month");
        const std::string day = date_result.front()["day"].template as<std::string>();
        const std::string month = date_result.front()["month"].template as<std::string>();
        const auto buckets = bucketsFor(owner_id, context_id, day, month);

        for (const auto& bucket : buckets) {
            lockBucket(transaction, bucket);
        }

        const auto existing = execParams(
            transaction,
            "SELECT owner_id, status FROM budget_reservations WHERE request_id = $1 FOR UPDATE",
            request_id);
        if (!existing.empty()) {
            if (existing.front()["owner_id"].template as<std::string>() == owner_id) {
                result.accepted = true;
                result.idempotent = true;
                result.reason = "idempotent";
            } else {
                result.accepted = false;
                result.idempotent = false;
                result.reason = "request unavailable";
            }
            return;
        }

        BudgetLimits effective_limits = limits;
        const auto policy = execParams(
            transaction,
            "SELECT global_limit, user_daily_limit, user_monthly_limit, session_limit "
            "FROM budget_policies WHERE owner_id = $1 FOR UPDATE",
            owner_id);
        if (!policy.empty()) {
            effective_limits.global = policy.front()["global_limit"].template as<std::int64_t>();
            effective_limits.user_daily = policy.front()["user_daily_limit"].template as<std::int64_t>();
            effective_limits.user_monthly = policy.front()["user_monthly_limit"].template as<std::int64_t>();
            effective_limits.session = policy.front()["session_limit"].template as<std::int64_t>();
        }

        const std::array<std::int64_t, 4> used = {
            readCounter(transaction, buckets[0], true),
            readCounter(transaction, buckets[1], true),
            readCounter(transaction, buckets[2], true),
            readCounter(transaction, buckets[3], true),
        };
        const std::array<std::int64_t, 4> effective = {
            effective_limits.global,
            effective_limits.user_daily,
            effective_limits.user_monthly,
            effective_limits.session,
        };
        for (std::size_t index = 0; index < buckets.size(); ++index) {
            if (wouldExceed(used[index], effective[index], estimated_tokens)) {
                result.accepted = false;
                result.idempotent = false;
                result.reason = buckets[index].reason;
                return;
            }
        }

        const auto inserted = execParams(
            transaction,
            "INSERT INTO budget_reservations "
            "(request_id, owner_id, context_id, estimated_tokens, status, created_at, updated_at) "
            "VALUES ($1, $2, $3, $4, 'reserved', NOW(), NOW()) "
            "ON CONFLICT (request_id) DO NOTHING RETURNING request_id",
            request_id, owner_id, context_id, estimated_tokens);
        if (inserted.empty()) {
            const auto existing_reservation = execParams(
                transaction,
                "SELECT owner_id FROM budget_reservations WHERE request_id = $1 FOR UPDATE",
                request_id);
            if (!existing_reservation.empty() &&
                existing_reservation.front()["owner_id"].template as<std::string>() == owner_id) {
                result.accepted = true;
                result.idempotent = true;
                result.reason = "idempotent";
            } else {
                result.accepted = false;
                result.idempotent = false;
                result.reason = "request unavailable";
            }
            return;
        }

        for (const auto& bucket : buckets) {
            addCounter(transaction, bucket, estimated_tokens);
        }
        result.accepted = true;
        result.idempotent = false;
        result.reason = "accepted";
    });
    return result;
}

bool PostgresBudgetRepository::setOwnerPolicy(const std::string& owner_id, const BudgetLimits& limits) {
    validateId(owner_id, "owner_id");
    validateLimits(limits);

    bool written = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO budget_policies "
            "(owner_id, global_limit, user_daily_limit, user_monthly_limit, session_limit, created_at, updated_at) "
            "VALUES ($1, $2, $3, $4, $5, NOW(), NOW()) "
            "ON CONFLICT (owner_id) DO UPDATE SET "
            "global_limit = EXCLUDED.global_limit, user_daily_limit = EXCLUDED.user_daily_limit, "
            "user_monthly_limit = EXCLUDED.user_monthly_limit, session_limit = EXCLUDED.session_limit, "
            "updated_at = NOW() RETURNING owner_id",
            owner_id, limits.global, limits.user_daily, limits.user_monthly, limits.session);
        written = !result.empty();
    });
    return written;
}

BudgetUsage PostgresBudgetRepository::usage(const std::string& owner_id, const std::string& context_id) {
    validateId(owner_id, "owner_id");
    validateId(context_id, "context_id");

    BudgetUsage result;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto date_result = transaction.exec(
            "SELECT CURRENT_DATE::text AS day, "
            "date_trunc('month', CURRENT_DATE)::date::text AS month");
        const std::string day = date_result.front()["day"].template as<std::string>();
        const std::string month = date_result.front()["month"].template as<std::string>();
        const auto buckets = bucketsFor(owner_id, context_id, day, month);
        result.global = readCounter(transaction, buckets[0], false);
        result.user_daily = readCounter(transaction, buckets[1], false);
        result.user_monthly = readCounter(transaction, buckets[2], false);
        result.session = readCounter(transaction, buckets[3], false);
    });
    return result;
}

BudgetUsage PostgresBudgetRepository::usageForOwner(const std::string& owner_id) {
    validateId(owner_id, "owner_id");
    BudgetUsage result;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto date_result = transaction.exec(
            "SELECT CURRENT_DATE::text AS day, "
            "date_trunc('month', CURRENT_DATE)::date::text AS month");
        const std::string day = date_result.front()["day"].template as<std::string>();
        const std::string month = date_result.front()["month"].template as<std::string>();
        const auto buckets = bucketsFor(owner_id, "", day, month);
        result.global = readCounter(transaction, buckets[0], false);
        result.user_daily = readCounter(transaction, buckets[1], false);
        result.user_monthly = readCounter(transaction, buckets[2], false);
        // session stays 0: the counter is context-bound, not account-level.
    });
    return result;
}

std::optional<BudgetLimits> PostgresBudgetRepository::getOwnerPolicy(
    const std::string& owner_id) {
    validateId(owner_id, "owner_id");
    std::optional<BudgetLimits> policy;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT global_limit, user_daily_limit, user_monthly_limit, session_limit "
            "FROM budget_policies WHERE owner_id = $1",
            owner_id);
        if (!result.empty()) {
            const auto& row = result.front();
            BudgetLimits limits;
            limits.global = row["global_limit"].template as<std::int64_t>();
            limits.user_daily = row["user_daily_limit"].template as<std::int64_t>();
            limits.user_monthly = row["user_monthly_limit"].template as<std::int64_t>();
            limits.session = row["session_limit"].template as<std::int64_t>();
            policy = limits;
        }
    });
    return policy;
}

}  // namespace agent_rpc::common
