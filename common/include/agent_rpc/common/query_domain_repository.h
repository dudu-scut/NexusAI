#pragma once

#include "agent_rpc/common/postgres_store.h"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace agent_rpc::common {

// Classifies runtime errors raised by the persistence layer without exposing
// libpqxx types to consumers (server targets do not include pqxx headers):
// true when the exception originated from PostgreSQL I/O.
bool isPostgresError(const std::exception& error);

struct ConversationRecord {
    std::string id;
    std::string owner_id;
    std::string title;
    std::string memory_summary;
    std::string created_at;
    std::string updated_at;
};

struct MessageRecord {
    std::string id;
    std::string owner_id;
    std::string conversation_id;
    std::string role;
    std::string content;
    std::int64_t sequence_no = 0;
    std::string created_at;
    std::string updated_at;
};

struct QueryLogRecord {
    std::string id;
    std::string owner_id;
    std::string conversation_id;
    std::string request_text;
    std::string route_decision;
    std::string execution_plan;
    std::string response_text;
    std::string model;
    std::string status;
    std::string created_at;
    std::string updated_at;
};

struct TraceRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string trace_payload;
    std::string status;
    std::string created_at;
    std::string updated_at;
};

struct TokenUsageLedgerRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string model;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    bool estimated = false;
    std::string cost_usd;
    std::string created_at;
    std::string updated_at;
};

struct FeedbackRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string agent_id;
    int rating = 0;
    std::string comment;
    std::string created_at;
    std::string updated_at;
};

struct RouteQualityRecord {
    std::string id;
    std::string owner_id;
    std::string agent_id;
    // V013 widened the uniqueness to (owner_id, agent_id, skill_name); the
    // skill dimension is part of the record and the upsert conflict target.
    std::string skill_name;
    std::int64_t sample_count = 0;
    std::string average_rating;
    std::string routing_weight;
    std::string created_at;
    std::string updated_at;
};

// B4 (P14d): one durable health-evaluation snapshot per agent, written by
// the 30s health_evaluation loop and reloaded at startup as the live-metrics
// baseline (freshness-gated; the in-memory ring buffer stays authoritative
// for per-call samples).
struct AgentHealthSnapshotRecord {
    std::string agent_id;
    std::string health_status;   // HEALTHY / DEGRADED / UNHEALTHY / UNKNOWN
    double success_rate = 0.0;
    double ema_latency_ms = 0.0;
    std::int64_t total_calls = 0;
};

// B5 (P17n): user profile row mirroring the V004 user_profiles columns —
// identity/preferences travel as JSONB text (same JSON documents the
// ProfileSummarizer stores in Redis).
struct UserProfileRecord {
    std::string user_id;
    std::string identity;          // JSON object text
    std::string preferences;       // JSON array text
    std::string context_snapshot;  // JSON object text
};

// C1 (V015): memory-domain durable records. user_memory_hints retires the
// last "[事实源] Redis" keys; user_memory_events is the append-only fact
// stream (方向5) written alongside every hint mutation.
struct UserMemoryHintRecord {
    std::string owner_id;
    std::string key;
    std::string value;
    std::string source = "segment";
};
struct UserMemoryEventRecord {
    std::string owner_id;
    std::string key;
    std::string value;
    std::string op;   // upsert / delete / conflict
};
// One batched mutation for applyUserMemoryHintBatch: the whole hint batch is
// applied in a single transaction so a mid-batch PG fault cannot leave a
// half-applied durable state while the fallback path rewrites the full set
// into the projection (P2-2).
struct UserMemoryHintOp {
    enum class Kind { Upsert, Delete };
    Kind kind = Kind::Upsert;
    std::string key;
    std::string value;
    std::string source;
    bool append_history = false;
    std::string event_op;   // "" = no event row; else upsert/delete/conflict
};

// [PR-E] Workflow-control records mirroring the V011 durable-domain tables.
struct SandboxRunRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string request_text;
    std::string response_text;
    std::string status;
    std::string created_at;
    std::string updated_at;
};

struct CompareRunRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string request_text;
    std::string results;  // JSONB text: per-agent result array
    std::string status;
    std::string created_at;
    std::string updated_at;
};

struct InterventionRecord {
    std::string id;
    std::string owner_id;
    // For pending interventions created by the autonomy gate this carries the
    // target agent id (the real query log id only exists once the deferred
    // execution runs); after execution it still references the gate target.
    std::string query_log_id;
    std::string state;  // pending | PROCEED | MODIFY | SKIP | ABORT
    std::string original_request;
    std::string edited_request;
    std::string decision;
    std::string created_at;
    std::string updated_at;
};

struct UndoActionRecord {
    std::string id;
    std::string owner_id;
    std::string resource_type;
    std::string resource_id;
    std::string action_payload;  // JSONB text
    int version = 1;
    std::string expires_at;
    std::string undone_at;
    bool expired = false;  // computed server-side (expires_at <= NOW())
    std::string created_at;
    std::string updated_at;
};

struct AutonomySettingRecord {
    std::string id;
    std::string owner_id;
    std::string agent_id;
    int level = 1;
    std::string created_at;
    std::string updated_at;
};

// [PR-E] Outcome of the owner-scoped intervention CAS transition.
enum class InterventionResolveOutcome {
    kResolved,       // pending -> decision happened exactly once
    kNotFound,       // unknown id or foreign owner (no existence leak)
    kAlreadyResolved // the record exists for the owner but is not pending
};

// [PR-E] Outcome of the atomic undo (CAS on undone_at + inverse restore in
// ONE transaction: a failed inverse never consumes the undo action).
enum class UndoOutcome {
    kApplied,       // undone_at set and the inverse committed together
    kNotFound,      // unknown action id or foreign owner
    kAlreadyUndone, // the action exists but was already consumed
    kInverseFailed  // rolled back: undone_at NOT consumed, caller may retry
};

// Names mirror the durable-domain tables while keeping all persistence behind
// one synchronous, tenant-scoped PostgreSQL repository.
class QueryDomainRepository final {
public:
    explicit QueryDomainRepository(PostgresStore& store);

    bool createConversation(const ConversationRecord& conversation);
    std::optional<ConversationRecord> getConversationById(const std::string& owner_id,
                                                            const std::string& conversation_id);
    std::vector<ConversationRecord> listConversations(const std::string& owner_id);

    // Confirms that a conversation exists for the owner, creating it on first
    // use. Idempotent for the same owner; returns false when the conversation
    // id is already owned by another user (or on database failure).
    bool ensureConversation(const std::string& owner_id, const std::string& conversation_id,
                            const std::string& title);

    // Returns false only when the owner/conversation/sequence tuple already
    // exists. Other PostgreSQL failures propagate to the caller.
    bool appendMessage(const MessageRecord& message);

    // Appends a message while assigning the next conversation sequence inside
    // a single transaction that first locks the owner's conversation row. The
    // sequence is never computed by callers (no MAX()+1 outside the
    // transaction). Returns the stored message, or std::nullopt when the
    // conversation does not belong to the owner.
    std::optional<MessageRecord> appendMessageAutoSequence(const std::string& owner_id,
                                                             const std::string& conversation_id,
                                                             const std::string& role,
                                                             const std::string& content);

    // Idempotent variant with a caller-chosen message id (deterministically
    // derived from the request id). When a row with that id already exists
    // the append is skipped WITHOUT consuming a sequence number, so retries
    // never duplicate history nor leave sequence gaps; the existing row is
    // returned. Unknown conversations or foreign owners still return
    // std::nullopt.
    std::optional<MessageRecord> appendMessageAutoSequence(const std::string& message_id,
                                                             const std::string& owner_id,
                                                             const std::string& conversation_id,
                                                             const std::string& role,
                                                             const std::string& content);

    std::vector<MessageRecord> listMessages(const std::string& owner_id,
                                             const std::string& conversation_id);

    bool createQueryLog(const QueryLogRecord& query_log);
    std::optional<QueryLogRecord> getQueryLogById(const std::string& owner_id,
                                                  const std::string& query_log_id);

    // [PR-G] Owner-scoped lookup of the most recent query log of one
    // conversation (ordered by creation time). Returns std::nullopt when the
    // conversation is unknown, belongs to another owner, or has no queries.
    std::optional<QueryLogRecord> getLatestQueryLogByConversation(
        const std::string& owner_id, const std::string& conversation_id);

    // Owner-scoped terminal update of an existing query log. This is a plain
    // UPDATE and never inserts: it returns false when the row does not exist
    // or belongs to another owner. Only the persisted terminal fields are
    // written (response_text, model, status, updated_at); every other record
    // field is ignored.
    bool updateQueryLog(const QueryLogRecord& query_log);

    bool createTrace(const TraceRecord& trace);
    std::optional<TraceRecord> getTraceById(const std::string& owner_id,
                                            const std::string& trace_id);

    // Owner-scoped terminal update of an existing trace. Same contract as
    // updateQueryLog: missing or cross-owner rows return false, no upsert.
    // Only the persisted terminal fields are written (trace_payload, status,
    // updated_at); every other record field is ignored.
    bool updateTrace(const TraceRecord& trace);

    // Appends one estimate-only ledger entry per id. A duplicate id (same
    // request retried) returns false instead of throwing, so finalize paths
    // can call it unconditionally; other PostgreSQL failures propagate.
    bool appendTokenUsageLedger(const TokenUsageLedgerRecord& usage);
    std::vector<TokenUsageLedgerRecord> listTokenUsageLedgerByOwner(const std::string& owner_id);

    bool createFeedback(const FeedbackRecord& feedback);

    std::optional<RouteQualityRecord> getRouteQuality(const std::string& owner_id,
                                                      const std::string& agent_id);
    bool upsertRouteQuality(const RouteQualityRecord& quality);

    // B4 (P14d): durable health-evaluation snapshots (V014
    // agent_health_snapshots). max_age_seconds filters stale rows on read so
    // the startup seeding never revives an ancient verdict.
    bool upsertAgentHealthSnapshot(const AgentHealthSnapshotRecord& snapshot);
    std::vector<AgentHealthSnapshotRecord> listAgentHealthSnapshots(
        std::int64_t max_age_seconds = 600);

    // B5 (P17n): user profile PG wiring (V004 user_profiles — the table
    // predates the Redis-side profile flow). upsert succeeds → caller
    // invalidates the Redis projection ("PG success → DEL Redis").
    bool upsertUserProfile(const UserProfileRecord& profile);
    std::optional<UserProfileRecord> getUserProfile(const std::string& user_id);

    // C1 (V015): long-term memory hints. append_history moves the previous
    // value into the JSONB history column (C2 方向3, fact-type keys).
    bool upsertUserMemoryHint(const UserMemoryHintRecord& hint,
                              bool append_history);
    // P2-1: cache-aside rebuild helper — inserts only when the (owner, key)
    // row does not exist yet, so a stale projection read can never overwrite
    // a newer durable value.
    bool insertUserMemoryHintIfAbsent(const UserMemoryHintRecord& hint);
    bool deleteUserMemoryHint(const std::string& owner_id, const std::string& key);
    std::vector<UserMemoryHintRecord> listUserMemoryHints(const std::string& owner_id);
    bool insertUserMemoryEvent(const UserMemoryEventRecord& event);
    bool applyUserMemoryHintBatch(const std::string& owner_id,
                                  const std::vector<UserMemoryHintOp>& ops);

    // C1 (V015): cross-agent summaries, keyed (context, target agent) with
    // target_agent_id "" = context-level. Reads keep the 7-day TTL semantics.
    bool upsertCrossAgentSummary(const std::string& context_id,
                                 const std::string& target_agent_id,
                                 const std::string& summary);
    std::vector<std::pair<std::string, std::string>> listCrossAgentSummaries(
        const std::string& context_id);

    // Short aliases retain the domain wording used by callers without adding
    // another SQL path or changing ownership semantics.
    bool appendTokenUsage(const TokenUsageLedgerRecord& usage) {
        return appendTokenUsageLedger(usage);
    }
    std::vector<TokenUsageLedgerRecord> listTokenUsageByOwner(const std::string& owner_id) {
        return listTokenUsageLedgerByOwner(owner_id);
    }

    // ---- [PR-E] workflow control: sandbox / compare / intervention / undo --

    bool createSandboxRun(const SandboxRunRecord& run);
    // Owner-scoped terminal update (status + response_text). Returns false
    // when the run does not exist or belongs to another owner.
    bool updateSandboxRun(const SandboxRunRecord& run);
    std::optional<SandboxRunRecord> getSandboxRunById(const std::string& owner_id,
                                                      const std::string& run_id);

    bool createCompareRun(const CompareRunRecord& run);
    // Owner-scoped terminal update (results JSONB + status).
    bool updateCompareRun(const CompareRunRecord& run);
    std::optional<CompareRunRecord> getCompareRunById(const std::string& owner_id,
                                                      const std::string& run_id);
    std::vector<CompareRunRecord> listCompareRunsByOwner(const std::string& owner_id);

    bool createIntervention(const InterventionRecord& intervention);
    std::optional<InterventionRecord> getInterventionById(const std::string& owner_id,
                                                          const std::string& intervention_id);
    // Single-statement CAS: pending -> decision for the owner's record. The
    // affected-row count decides success; a follow-up owner-scoped existence
    // check distinguishes NOT_FOUND from ALREADY_EXISTS inside the same
    // transaction.
    InterventionResolveOutcome resolveIntervention(const std::string& owner_id,
                                                   const std::string& intervention_id,
                                                   const std::string& decision,
                                                   const std::string& edited_request);
    // The expiry window (24 hours) is assigned by SQL at insert time.
    bool createUndoAction(const UndoActionRecord& action);
    std::optional<UndoActionRecord> getUndoActionById(const std::string& owner_id,
                                                      const std::string& action_id);
    // Atomic undo: single transaction that (1) CAS-sets undone_at while it is
    // still NULL and (2) restores the intervention to pending. Both commit or
    // roll back together, so a failed inverse never burns the action and the
    // owner can retry. kInverseFailed also covers transient PG errors.
    UndoOutcome undoRestoreIntervention(const std::string& owner_id,
                                        const std::string& action_id,
                                        const std::string& intervention_id);

    // PostgreSQL upsert keyed on (owner_id, agent_id); a repeated set updates
    // the same row in place and never duplicates it.
    bool upsertAutonomySetting(const std::string& owner_id, const std::string& agent_id,
                               int level);
    // std::nullopt means "no setting"; callers apply their own conservative
    // default (the autonomy gate treats it as level 1 = confirmation).
    std::optional<AutonomySettingRecord> getAutonomySetting(const std::string& owner_id,
                                                            const std::string& agent_id);

private:
    PostgresStore& store_;
};

using Conversation = ConversationRecord;
using Message = MessageRecord;
using QueryLog = QueryLogRecord;
using Trace = TraceRecord;
using TokenUsageLedger = TokenUsageLedgerRecord;
using Feedback = FeedbackRecord;
using RouteQuality = RouteQualityRecord;
using SandboxRun = SandboxRunRecord;
using CompareRun = CompareRunRecord;
using Intervention = InterventionRecord;
using UndoAction = UndoActionRecord;
using AutonomySetting = AutonomySettingRecord;

}  // namespace agent_rpc::common
