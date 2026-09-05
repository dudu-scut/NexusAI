/**
 * @file task_planner.h
 * @brief TaskPlanner — decomposes user queries into multi-agent execution plans
 *
 * Uses LLM to analyze whether a query requires single-agent or multi-agent
 * execution.  For multi-agent queries, produces an ExecutionPlan with
 * dependency-aware SubTasks forming a DAG.
 */

#pragma once

#include "agent_rpc/common/env_loader.h"
#include <a2a/llm_client.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_rpc {
namespace orchestrator {

class AgentRouter;  // forward declaration for resolveAgents()

/**
 * @brief A candidate agent for a subtask with confidence score.
 * Used to present Top-3 choices to the user.
 */
struct CandidateAgent {
    std::string agent_id;
    std::string agent_name;
    double confidence = 0.0;               // Routing confidence (0.0–1.0)
    // P12(a): provenance of `confidence`. "embedding" = real cosine
    // similarity from the router's embedding tier; "ranking" = the
    // 1.0 - 0.15*rank placeholder (LLM/IDF/fallback tiers carry no numeric
    // confidence). Consumers must label the value accordingly.
    std::string confidence_source = "ranking";
};

struct SubTask {
    // B3/P20-7: does executing this task mutate external state? The
    // planning LLM labels write-shaped tasks ("effect": "write"); a timed-out
    // SideEffect task is reported as UNCERTAIN (may have applied) instead of
    // plain FAILED. Default is ReadOnly — an unlabelled task keeps the
    // historical FAILED semantics (documented residual risk: an unlabelled
    // write task that times out is reported failed exactly as before).
    enum class Effect { ReadOnly, SideEffect };
    std::string id;                        // "t1", "t2", ...
    std::string description;               // Prompt sent to the Agent
    std::string required_skill;            // Skill needed
    std::vector<std::string> depends_on;   // IDs of prerequisite subtasks
    std::string preferred_agent_id;        // Pre-resolved agent (set by resolveAgents)
    std::string preferred_agent_name;      // Agent name for logging
    std::vector<CandidateAgent> candidate_agents; // Top-3 candidates
    Effect effect = Effect::ReadOnly;      // write-shaped task marker (B3)
};

struct ExecutionPlan {
    std::string original_query;            // Original user request
    std::vector<SubTask> tasks;            // Ordered subtask list
    bool is_single_agent = true;           // true → fast-path single Agent
    std::string single_agent_skill;        // Skill for the single-agent path
    std::string single_agent_id;           // Pre-resolved agent ID for single-agent path
    std::string single_agent_name;         // Agent name for single-agent path
    // P12(a): real routing confidence for the single-agent decision
    // (meaningful only when single_agent_confidence_source == "embedding").
    double single_agent_confidence = 0.0;
    std::string single_agent_confidence_source = "ranking";
    // P13(d)/A2: number of subtasks dropped during plan parsing (missing
    // id/description), worst-observed across the retry attempts. Populated
    // by plan(); consumed by the aggregator so the final answer can tell
    // the user that some requirements were not covered.
    int dropped_tasks = 0;
};

// ── Configuration ──────────────────────────────────────────────────────────

struct TaskPlannerConfig {
    std::string api_key;
    std::string model    = agent_rpc::common::envOrDefault("LLM_MODEL", "deepseek-v4-flash");
    std::string api_url  = agent_rpc::common::envOrDefault("LLM_API_URL", "https://api.deepseek.com/v1/chat/completions");
};

// ── TaskPlanner class ──────────────────────────────────────────────────────

class TaskPlanner {
public:
    explicit TaskPlanner(const TaskPlannerConfig& config);

    /**
     * Test seam: construct with an injected LLM client (e.g. a scripted fake)
     * so plan() can be exercised without a real network call.
     */
    TaskPlanner(const TaskPlannerConfig& config, std::unique_ptr<LLMClient> llm_client);

    /**
     * Analyze a user query and produce an execution plan.
     * @param query              User's question / request
     * @param available_skills   skill → description map from the registry
     * @return ExecutionPlan with is_single_agent flag and optional subtask DAG
     */
    ExecutionPlan plan(const std::string& query,
                       const std::unordered_map<std::string, std::string>& available_skills,
                       int llm_timeout_seconds = 20,
                       const std::atomic<bool>* abort_flag = nullptr);

    /**
     * Pre-resolve agents for each subtask in the plan using AgentRouter.
     * Should be called after plan() to populate preferred_agent_id fields.
     * For single-agent plans, also resolves single_agent_id.
     *
     * @param plan    ExecutionPlan from plan() — modified in place
     * @param router  AgentRouter to use for skill → agent resolution
     */
    void resolveAgents(ExecutionPlan& plan, AgentRouter& router);

    /**
     * Parse a raw LLM plan response into an ExecutionPlan.
     * Public so tests can feed fabricated JSON directly without an LLM call.
     *
     * @param response  Raw LLM response text (markdown fences tolerated)
     * @param query     Original user query (stored as plan.original_query)
     * @return Parsed ExecutionPlan
     */
    ExecutionPlan parsePlanResponse(const std::string& response,
                                    const std::string& query) const;

private:
    std::string buildPlanningPrompt(
        const std::string& query,
        const std::unordered_map<std::string, std::string>& available_skills) const;

    /**
     * Parse a raw LLM plan response, reporting how many subtasks were dropped
     * because of missing critical fields (empty id or description). The
     * two-argument overload above forwards here with an ignored counter.
     */
    ExecutionPlan parsePlanResponse(const std::string& response,
                                    const std::string& query,
                                    int& dropped_count) const;

    TaskPlannerConfig config_;
    std::unique_ptr<LLMClient> llm_client_;
};

} // namespace orchestrator
} // namespace agent_rpc
