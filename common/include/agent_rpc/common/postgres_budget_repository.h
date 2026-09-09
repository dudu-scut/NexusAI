#pragma once

#include "agent_rpc/common/postgres_store.h"

#include <cstdint>
#include <optional>
#include <string>

namespace agent_rpc::common {

// A value of zero means that the corresponding bucket is unlimited.
struct BudgetLimits {
    std::int64_t global = 0;
    std::int64_t user_daily = 0;
    std::int64_t user_monthly = 0;
    std::int64_t session = 0;
};

struct BudgetReservationResult {
    bool accepted = false;
    bool idempotent = false;
    std::string reason;
};

using BudgetReserveResult = BudgetReservationResult;
using ReserveResult = BudgetReservationResult;

struct BudgetUsage {
    std::int64_t global = 0;
    std::int64_t user_daily = 0;
    std::int64_t user_monthly = 0;
    std::int64_t session = 0;
};

using BudgetUsageSnapshot = BudgetUsage;

// Persists token reservations and all four counters in PostgreSQL.  The
// caller supplies the default limits; an owner policy, when present, takes
// precedence for that owner.
class PostgresBudgetRepository final {
public:
    explicit PostgresBudgetRepository(PostgresStore& store);

    BudgetReservationResult reserve(const std::string& owner_id,
                                    const std::string& context_id,
                                    const std::string& request_id,
                                    std::int64_t estimated_tokens,
                                    const BudgetLimits& limits);

    bool setOwnerPolicy(const std::string& owner_id, const BudgetLimits& limits);

    BudgetUsage usage(const std::string& owner_id, const std::string& context_id);

    BudgetUsage getUsage(const std::string& owner_id, const std::string& context_id) {
        return usage(owner_id, context_id);
    }

    // Owner-scoped read for budget dashboards: global/daily/monthly buckets
    // only — the session counter is context-bound and not part of an
    // account overview (it stays 0 here).
    BudgetUsage usageForOwner(const std::string& owner_id);

    // Returns the owner policy when one exists; nullopt means the caller
    // falls back to the environment defaults.
    std::optional<BudgetLimits> getOwnerPolicy(const std::string& owner_id);

private:
    PostgresStore& store_;
};

using BudgetRepository = PostgresBudgetRepository;

}  // namespace agent_rpc::common
