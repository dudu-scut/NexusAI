-- V014: agent health snapshots (batch 8, item B4).
-- One row per agent, UPSERTed by the 30s health_evaluation loop so the
-- in-memory live-metrics ring buffer (registry, process-local by design)
-- survives restarts: startup reloads fresh snapshots as a baseline instead
-- of re-learning every agent's health from a cold start. The ring buffer
-- itself stays in memory (high-frequency writes); PostgreSQL only stores
-- the evaluation verdicts. Append-only; V001–V013 are never edited.
-- All statements are idempotent so tests may re-apply this migration directly.

-- ============================================================================
-- 1. agent_health_snapshots: latest evaluation verdict per agent.
--    P14(d) storage half: the evaluation loop already writes markAgent*/
--    recordAgentCall facts; this table is the durable projection of the
--    evaluateAllHealth verdict for restart recovery and (future) dashboards.
-- ============================================================================

CREATE TABLE IF NOT EXISTS agent_health_snapshots (
    agent_id TEXT PRIMARY KEY,
    health_status TEXT NOT NULL DEFAULT 'UNKNOWN',
    success_rate DOUBLE PRECISION NOT NULL DEFAULT 0,
    ema_latency_ms DOUBLE PRECISION NOT NULL DEFAULT 0,
    total_calls BIGINT NOT NULL DEFAULT 0,
    sampled_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS agent_health_snapshots_sampled_at_idx
    ON agent_health_snapshots (sampled_at DESC);
