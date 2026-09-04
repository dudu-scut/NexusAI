#!/usr/bin/env bash
# Static redline self-check for Batch-1 memory-loop patches.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SVC=server/src/ai_query_service.cpp
MAH=server/src/multi_agent_handler.cpp

count() { grep -o -F "$2" "$1" 2>/dev/null | wc -l; }

echo "buildSystemContextFromPg(            : $(count "$SVC" 'buildSystemContextFromPg(')"
echo "usage.estimated = true               : $(count "$SVC" 'usage.estimated = true')"
echo "!request->sandbox()                  : $(count "$SVC" '!request->sandbox()')"
echo "updateUserMemoryFromHints            : $(count "$SVC" 'updateUserMemoryFromHints')"
echo "BudgetMiddleware::checkAndDeduct (0) : $(count "$SVC" 'BudgetMiddleware::checkAndDeduct')"
echo "Query pipeline crashed: (2)          : $(count "$SVC" 'Query pipeline crashed: ')"
echo "Query unavailable msg (2)            : $(count "$SVC" 'Query unavailable: persistence layer error')"
echo "Query failed unexpectedly (2)        : $(count "$SVC" 'Query failed unexpectedly')"
echo "abortDurableRun(run, ...) (>=2)      : $(count "$SVC" 'abortDurableRun(run, error.what())')"
echo "setLastAgent(context_id, \"default\")(0): $(count "$SVC" 'setLastAgent(context_id, "default")')"
echo "handler set_event_type(complete) (0) : $(count "$MAH" 'set_event_type("complete")')"
echo "handler set_event_type(error) (0)    : $(count "$MAH" 'set_event_type("error")')"
echo "---- Batch-2 redlines ----"
TC=common/include/agent_rpc/common/trace_context.h
TX=orchestrator/src/task_executor.cpp
echo "usage.estimated = true (>=1)         : $(count "$SVC" 'usage.estimated = true')"
echo "two-arg init text preserved (=1)     : $(count "$TC" 'static void init(const std::string& user_id, const std::string& context_id) {')"
echo "three-arg init overload exists (>=1) : $(count "$TC" 'const std::string& existing_trace_id)')"
echo "propagation switch in executor (>=1) : $(count "$TX" 'NEXUSAI_TRACE_PARENT_PROPAGATION')"
echo "legacy init kept for switch-off (>=1): $(count "$TX" 'TraceContext::init(parent_user_id, "")')"
echo "---- Batch-3 redlines ----"
echo "persistTraceSpansToRedis in server (0): $(grep -r -o -F 'persistTraceSpansToRedis' server/ 2>/dev/null | wc -l)"
echo "trace:<id>:spans per-query key (0)   : $(grep -r -o -E '"trace:" \+ |":spans"' server/ 2>/dev/null | wc -l)"
echo "batch span path in main.cpp (>=1)     : $(count server/src/main.cpp 'span_batch_flush')"
echo "---- Batch-4 redlines ----"
AR=orchestrator/src/agent_router.cpp
echo "agent_router.cpp CANARY (0)          : $(count "$AR" 'CANARY')"
echo "agent_router.cpp DEPRECATED (0)      : $(count "$AR" 'DEPRECATED')"
echo "agent_router.cpp legacy feedback key (0): $(count "$AR" 'feedback:" + agent_id')"
echo "agent_router.cpp quality_provider_ (>=1): $(count "$AR" 'quality_provider_')"
echo "agent_router.cpp P8 switch wired (>=1): $(count "$AR" 'NEXUSAI_ROUTER_LB_STRATEGY')"
echo "ai_query_service.cpp P7 switch wired (>=1): $(count "$SVC" 'NEXUSAI_EMBEDDING_ROUTER')"
echo "---- Batch-4 zero-diff gate: CMake files and main.cpp ----"
git diff --stat -- CMakeLists.txt orchestrator/CMakeLists.txt server/CMakeLists.txt common/CMakeLists.txt mcp/CMakeLists.txt tests/CMakeLists.txt server/src/main.cpp | cat
echo "(empty output above = zero diff)"
echo "---- Batch-5 redlines (P10 fast path) ----"
echo "handler set_event_type(complete) (0) : $(count "$MAH" 'set_event_type("complete")')"
echo "handler set_event_type(error) (0)    : $(count "$MAH" 'set_event_type("error")')"
echo "handler P10 switch wired (>=1)       : $(count "$MAH" 'NEXUSAI_SINGLE_INTENT_FAST_PATH')"
echo "handler tryBuildFastPathPlan (>=3)   : $(count "$MAH" 'tryBuildFastPathPlan')"
echo "handler fast-path retry (>=2)        : $(count "$MAH" 'retrying with full planning')"
echo "router resolveHighConfidenceSkill (>=2): $(count "$AR" 'resolveHighConfidenceSkill')"
echo "---- memory_summary occurrences in server/src (read-only allowed) ----"
grep -rn 'memory_summary' server/src/ || echo "(none)"
echo "---- any UPDATE ... memory_summary anywhere (must be none) ----"
grep -rniE 'update[^\n]*memory_summary|memory_summary[^\n]*=' server/src common/src db/migrations sql 2>/dev/null | grep -viE 'select|read' || echo "(none)"
echo "---- tests/CMakeLists.txt git diff ----"
git diff --stat -- tests/CMakeLists.txt | cat
echo "---- changed files ----"
git status --short | cat
