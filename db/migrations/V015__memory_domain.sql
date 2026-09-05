-- V015: memory-domain durable records (batch 8, items C1 + C2 方向3/5).
-- Retires the last "[事实源] Redis" keys: long-term memory hints and
-- cross-agent summaries move to PostgreSQL; Redis keys become projections
-- (write path: PG success → DEL key; read path: PG miss → Redis → backfill).
-- The event table records the append-only fact stream (方向5). Append-only;
-- V001–V014 are never edited. All statements are idempotent.

-- ============================================================================
-- 1. user_memory_hints: Tier-2 long-term memory, keyed (owner, key).
--    history (方向3): JSON array of previous values for fact-type keys —
--    a fact overwrite moves the old value here instead of destroying it.
-- ============================================================================
CREATE TABLE IF NOT EXISTS user_memory_hints (
    owner_id TEXT NOT NULL,
    mem_key TEXT NOT NULL,
    mem_value TEXT NOT NULL DEFAULT '',
    source TEXT NOT NULL DEFAULT 'segment',
    history JSONB NOT NULL DEFAULT '[]',
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (owner_id, mem_key)
);

-- ============================================================================
-- 2. cross_agent_summaries: Tier-3 summaries, keyed (context, target agent).
--    target_agent_id '' is the context-level summary (pre-B3 general form);
--    per-agent specializations keep their own row. Reads filter to the
--    7-day window to preserve the historical TTL semantics.
-- ============================================================================
CREATE TABLE IF NOT EXISTS cross_agent_summaries (
    context_id TEXT NOT NULL,
    target_agent_id TEXT NOT NULL DEFAULT '',
    summary TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (context_id, target_agent_id)
);

-- ============================================================================
-- 3. user_memory_events (方向5): append-only fact stream. Writes always
--    INSERT here (upsert/delete/conflict); the effective view is projected
--    into user_memory_hints. Replay/projection recalculation is a future
--    BackgroundScheduler concern.
-- ============================================================================
CREATE TABLE IF NOT EXISTS user_memory_events (
    id BIGSERIAL PRIMARY KEY,
    owner_id TEXT NOT NULL,
    mem_key TEXT NOT NULL,
    mem_value TEXT NOT NULL DEFAULT '',
    op TEXT NOT NULL,                -- upsert / delete / conflict
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS user_memory_events_owner_created_idx
    ON user_memory_events (owner_id, created_at DESC);
