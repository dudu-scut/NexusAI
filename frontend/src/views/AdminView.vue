<template>
  <div class="admin-view">
    <div class="admin-header">
      <div class="header-brand">
        <router-link to="/" class="back-link">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
        </router-link>
        <h1>Admin Dashboard</h1>
      </div>
      <div class="header-actions">
        <button class="btn-refresh" @click="refreshAll" :disabled="loading">
          {{ loading ? 'Refreshing...' : 'Refresh' }}
        </button>
      </div>
    </div>

    <!-- Tab Navigation -->
    <div class="tab-bar">
      <button
        v-for="tab in tabs"
        :key="tab.id"
        class="tab"
        :class="{ active: activeTab === tab.id }"
        @click="activeTab = tab.id"
      >
        <span v-html="tab.icon"></span>
        {{ tab.label }}
      </button>
    </div>

    <div class="admin-content">
      <!-- === Dashboard Tab === -->
      <div v-if="activeTab === 'dashboard'" class="tab-content">
        <div class="stats-grid">
          <div class="stat-card">
            <div class="stat-icon" style="background:rgba(59,130,246,0.12);color:#3b82f6;">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/></svg>
            </div>
            <div class="stat-info">
              <span class="stat-value">{{ agents.length }}</span>
              <span class="stat-label">Registered Agents</span>
            </div>
          </div>
          <div class="stat-card">
            <div class="stat-icon" style="background:rgba(34,197,94,0.12);color:#22c55e;">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></svg>
            </div>
            <div class="stat-info">
              <span class="stat-value">{{ healthyCount }}</span>
              <span class="stat-label">Healthy</span>
            </div>
          </div>
          <div class="stat-card">
            <div class="stat-icon" style="background:rgba(245,158,11,0.12);color:#f59e0b;">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
            </div>
            <div class="stat-info">
              <span class="stat-value">{{ degradedCount }}</span>
              <span class="stat-label">Degraded</span>
            </div>
          </div>
          <div class="stat-card">
            <div class="stat-icon" style="background:rgba(239,68,68,0.12);color:#ef4444;">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>
            </div>
            <div class="stat-info">
              <span class="stat-value">{{ offlineCount }}</span>
              <span class="stat-label">Offline</span>
            </div>
          </div>
        </div>

        <!-- Agent Health Table -->
        <div class="section">
          <h3>Agent Health Status</h3>
          <div class="table-wrap">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Agent</th><th>Status</th><th>Success Rate</th><th>Avg Latency</th><th>Total Requests</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="agent in agents" :key="agent.id">
                  <td>
                    <div class="agent-cell">
                      <span class="agent-name">{{ agent.name }}</span>
                      <span class="agent-host">{{ agent.host }}:{{ agent.port }}</span>
                    </div>
                  </td>
                  <td>
                    <span class="status-light" :class="getHealthClass(agent)"></span>
                    {{ getHealthLabel(agent) }}
                  </td>
                  <td>
                    <div class="bar-cell">
                      <div class="bar-bg"><div class="bar-fill good" :style="{ width: getSuccessRate(agent) + '%' }"></div></div>
                      <span class="bar-text">{{ getSuccessRate(agent) }}%</span>
                    </div>
                  </td>
                  <td>{{ getAvgLatency(agent) }}{{ agent.metrics?.avg_latency_ms != null ? 'ms' : '' }}</td>
                  <td>{{ agent.metrics?.total_requests ?? 0 }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <!-- === Budget Tab === -->
      <div v-if="activeTab === 'budget'" class="tab-content">
        <div class="section">
          <h3>Token Budget Overview</h3>
          <!-- Global shared bucket: only rendered when a global cap exists or
               tokens were consumed; 0 limit means unlimited. -->
          <div v-if="globalLimit > 0 || globalUsed > 0" class="budget-global">
            <span class="budget-global-label">Global (shared across users)</span>
            <span class="budget-global-values">
              limit <strong>{{ formatLimit(globalLimit) }}</strong>
              · used <strong>{{ formatTokens(globalUsed) }}</strong>
              <span v-if="globalLimit > 0">· remaining <strong>{{ formatTokens(globalRemaining) }}</strong></span>
            </span>
          </div>
          <div class="budget-cards">
            <div class="budget-card">
              <div class="budget-ring">
                <svg width="100" height="100" viewBox="0 0 36 36">
                  <path d="M18 2.0845 a 15.9155 15.9155 0 0 1 0 31.831 a 15.9155 15.9155 0 0 1 0 -31.831" fill="none" stroke="rgba(255,255,255,0.08)" stroke-width="3"/>
                  <path :d="dailyArc" fill="none" stroke="#6366f1" stroke-width="3" stroke-linecap="round"/>
                </svg>
                <div class="ring-text">
                  <span class="ring-value">{{ dailyUsedPercent }}%</span>
                  <span class="ring-label">Today</span>
                </div>
              </div>
              <div class="budget-details">
                <div class="budget-row"><span>Daily Limit</span><span>{{ formatLimit(dailyLimit) }}</span></div>
                <div class="budget-row"><span>Used</span><span>{{ formatTokens(dailyUsed) }}</span></div>
                <div class="budget-row"><span>Remaining</span><span class="remaining">{{ dailyLimit === 0 ? '—' : formatTokens(Math.max(0, dailyLimit - dailyUsed)) }}</span></div>
              </div>
            </div>
            <div class="budget-card">
              <div class="budget-details full-width">
                <div class="budget-row"><span>Monthly Limit</span><span>{{ formatLimit(monthlyLimit) }}</span></div>
                <div class="budget-row"><span>Used</span><span>{{ formatTokens(monthlyUsed) }}</span></div>
                <div class="budget-row"><span>Remaining</span><span class="remaining">{{ monthlyLimit === 0 ? '—' : formatTokens(Math.max(0, monthlyLimit - monthlyUsed)) }}</span></div>
                <div class="budget-row muted"><span>Reset Time</span><span>{{ resetTime }}</span></div>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- === Replay Tab === -->
      <div v-if="activeTab === 'replay'" class="tab-content">
        <div class="section">
          <h3>Query Replay</h3>
          <div class="replay-form">
            <div class="input-group">
              <label>Trace ID</label>
              <input v-model="replayTraceId" placeholder="Enter trace_id..." class="text-input" />
            </div>
            <div class="input-group">
              <label>Mode</label>
              <select v-model="replayMode" class="text-input">
                <option value="EXACT">Exact Replay</option>
                <option value="ROUTE">Re-route</option>
              </select>
            </div>
            <button class="btn-primary" @click="replayQuery" :disabled="!replayTraceId.trim() || replayLoading">
              {{ replayLoading ? 'Running…' : 'Run Replay' }}
            </button>
          </div>
          <div v-if="replayError" class="replay-result error">
            <div class="replay-header">Replay Error</div>
            <pre class="replay-json">{{ replayError }}</pre>
          </div>
          <div v-if="replayResult" class="replay-result">
            <div class="replay-header">Replay Result</div>
            <pre class="replay-json">{{ replayResult }}</pre>
          </div>
        </div>
      </div>

      <!-- Cron/Canary tabs removed — backend capabilities were deleted. -->
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useAgentsStore } from '../stores/agents'
import { replayQuery as replayQueryRpc, getBudgetSummary } from '../services/grpc-client'
import type { AgentDisplayInfo } from '../types/proto'

const agentsStore = useAgentsStore()
const loading = ref(false)
const activeTab = ref('dashboard')

const tabs = [
  { id: 'dashboard', label: 'Dashboard', icon: '📊' },
  { id: 'budget', label: 'Budget', icon: '💰' },
  { id: 'replay', label: 'Replay', icon: '🔁' },
]

const agents = computed(() => agentsStore.agents)
// Health verdicts come from agent_registry via GetAgents.health_status
// (written by the 30s health-evaluation task); agents without a verdict yet
// count nowhere and show as Unknown in the table.
const healthyCount = computed(() => agents.value.filter(a => a.healthStatus === 'HEALTHY').length)
const degradedCount = computed(() => agents.value.filter(a => a.healthStatus === 'DEGRADED').length)
const offlineCount = computed(() =>
  agents.value.filter(a => a.healthStatus === 'UNHEALTHY' || a.healthStatus === 'offline').length,
)

function getHealthClass(a: AgentDisplayInfo): string {
  switch (a.healthStatus) {
    case 'HEALTHY': return 'healthy'
    case 'DEGRADED': return 'degraded'
    case 'UNHEALTHY':
    case 'offline': return 'unhealthy'
    default: return 'unknown'
  }
}

function getHealthLabel(a: AgentDisplayInfo): string {
  switch (a.healthStatus) {
    case 'HEALTHY': return 'Healthy'
    case 'DEGRADED': return 'Degraded'
    case 'UNHEALTHY':
    case 'offline': return 'Offline'
    default: return 'Unknown'
  }
}

function getSuccessRate(a: AgentDisplayInfo): number {
  return Math.round((a.metrics?.success_rate ?? 0) * 100)
}

function getAvgLatency(a: AgentDisplayInfo): number | string {
  return a.metrics?.avg_latency_ms ?? '--'
}

// Budget — real values arrive via GetBudgetSummary (owner-scoped PG read of
// budget_counters + policy/env limits); these refs used to stay 0 forever on
// a mock-only page (deep-review fr-*).
const globalLimit = ref(0)
const globalUsed = ref(0)
const dailyLimit = ref(0)
const dailyUsed = ref(0)
const monthlyLimit = ref(0)
const monthlyUsed = ref(0)
const budgetLoading = ref(false)

const globalRemaining = computed(() => Math.max(0, globalLimit.value - globalUsed.value))
// daily/monthly remaining are rendered inline with a 0=unlimited guard.
const dailyUsedPercent = computed(() => dailyLimit.value > 0 ? Math.round((dailyUsed.value / dailyLimit.value) * 100) : 0)
const resetTime = computed(() => {
  const d = new Date()
  d.setDate(d.getDate() + 1)
  d.setHours(0, 0, 0, 0)
  return d.toLocaleString()
})

const dailyArc = computed(() => {
  const pct = dailyUsedPercent.value
  const angle = (pct / 100) * 360
  const r = 15.9155
  const rad = (angle - 90) * Math.PI / 180
  const x = 18 + r * Math.cos(rad)
  const y = 18 + r * Math.sin(rad)
  const large = angle > 180 ? 1 : 0
  return `M 18 2.0845 A 15.9155 15.9155 0 ${large} 1 ${x} ${y}`
})

function formatTokens(n: number): string {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(1) + 'M'
  if (n >= 1_000) return (n / 1_000).toFixed(1) + 'K'
  return n.toString()
}

// limit 0 means unlimited (the global default) and renders as such.
function formatLimit(n: number): string {
  return n === 0 ? 'Unlimited' : formatTokens(n)
}

async function loadBudget() {
  budgetLoading.value = true
  try {
    const resp = await getBudgetSummary()
    if (!resp) return
    globalLimit.value = Number(resp.global?.limit ?? 0)
    globalUsed.value = Number(resp.global?.used ?? 0)
    dailyLimit.value = Number(resp.daily?.limit ?? 0)
    dailyUsed.value = Number(resp.daily?.used ?? 0)
    monthlyLimit.value = Number(resp.monthly?.limit ?? 0)
    monthlyUsed.value = Number(resp.monthly?.used ?? 0)
  } finally {
    budgetLoading.value = false
  }
}

// Replay
const replayTraceId = ref('')
const replayMode = ref<'EXACT' | 'ROUTE'>('EXACT')
const replayResult = ref('')
const replayLoading = ref(false)
const replayError = ref('')

async function replayQuery() {
  replayLoading.value = true
  replayError.value = ''
  replayResult.value = ''
  try {
    const resp = await replayQueryRpc(
      replayTraceId.value.trim(),
      replayMode.value === 'EXACT' ? 'exact' : 'route',
    )
    replayResult.value = JSON.stringify(resp, null, 2)
  } catch (e) {
    replayError.value = e instanceof Error ? e.message : String(e)
  } finally {
    replayLoading.value = false
  }
}

// Cron/Canary state and handlers removed — the backend capabilities
// were deleted, so no local demo arrays may remain.

async function refreshAll() {
  loading.value = true
  await agentsStore.fetchAgents()
  loading.value = false
}

onMounted(() => {
  agentsStore.startPolling(15000)
  loadBudget()
})

onUnmounted(() => {
  agentsStore.stopPolling()
})
</script>

<style scoped>
@import "../styles/design-tokens.css";

.admin-view {
  display: flex;
  flex-direction: column;
  height: 100vh;
  background: var(--bg-primary);
}

.admin-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 24px;
  background: var(--bg-elevated);
  border-bottom: 1px solid var(--border-default);
}

.header-brand {
  display: flex;
  align-items: center;
  gap: 12px;
}

.back-link {
  display: flex;
  align-items: center;
  color: var(--text-secondary);
  padding: 4px;
  border-radius: var(--radius-sm);
  transition: all 0.15s;
}

.back-link:hover {
  background: var(--glass-bg-hover);
  color: var(--text-primary);
}

.header-brand h1 {
  font-size: 18px;
  font-weight: 700;
  margin: 0;
  color: var(--text-primary);
}

.btn-refresh {
  padding: 6px 16px;
  border-radius: var(--radius-sm);
  font-size: 13px;
  cursor: pointer;
  border: 1px solid var(--border-default);
  background: var(--bg-elevated);
  color: var(--text-secondary);
  font-weight: 500;
  transition: all 0.15s;
}

.btn-refresh:hover:not(:disabled) { background: var(--glass-bg-hover); color: var(--text-primary); }
.btn-refresh:disabled { opacity: 0.6; cursor: not-allowed; }

/* Tabs */
.tab-bar {
  display: flex;
  gap: 0;
  padding: 0 24px;
  background: var(--bg-elevated);
  border-bottom: 1px solid var(--border-default);
}

.tab {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 12px 18px;
  border: none;
  background: transparent;
  font-size: 13px;
  font-weight: 500;
  color: var(--text-secondary);
  cursor: pointer;
  border-bottom: 2px solid transparent;
  transition: all 0.15s;
}

.tab:hover { color: var(--text-primary); }

.tab.active {
  color: var(--brand-primary);
  border-bottom-color: var(--brand-primary);
}

.admin-content {
  flex: 1;
  overflow-y: auto;
  padding: 24px;
}

.tab-content {
  max-width: 1100px;
  animation: tabFadeIn 0.25s var(--ease-out);
}

@keyframes tabFadeIn {
  from { opacity: 0; transform: translateY(6px); }
  to { opacity: 1; transform: translateY(0); }
}

/* Stats Grid */
.stats-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 16px;
  margin-bottom: 28px;
}

.stat-card {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 16px 20px;
  background: var(--bg-elevated);
  border: 1px solid var(--border-default);
  border-radius: var(--radius-md);
}

.stat-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 44px;
  height: 44px;
  border-radius: 10px;
  flex-shrink: 0;
}

.stat-info {
  display: flex;
  flex-direction: column;
}

.stat-value {
  font-size: 22px;
  font-weight: 700;
  color: var(--text-primary);
}

.stat-label {
  font-size: 12px;
  color: var(--text-secondary);
  margin-top: 1px;
}

/* Sections */
.section {
  background: var(--bg-elevated);
  border: 1px solid var(--border-default);
  border-radius: var(--radius-md);
  padding: 20px;
  margin-bottom: 20px;
}

.section h3 {
  font-size: 15px;
  font-weight: 600;
  color: var(--text-primary);
  margin: 0 0 16px;
}

/* Tables */
.table-wrap { overflow-x: auto; }

.data-table { width: 100%; border-collapse: collapse; font-size: 13px; }
.data-table th { padding: 10px 12px; text-align: left; font-weight: 600; color: var(--text-secondary); font-size: 11px; text-transform: uppercase; letter-spacing: 0.5px; border-bottom: 2px solid var(--border-default); }
.data-table td { padding: 10px 12px; border-bottom: 1px solid var(--border-subtle); color: var(--text-secondary); }

.data-table tbody tr {
  transition: background var(--duration-fast);
}
.data-table tbody tr:hover {
  background: var(--glass-bg-hover);
}

.agent-cell { display: flex; flex-direction: column; }
.agent-name { font-weight: 600; font-size: 13px; color: var(--text-primary); }
.agent-host { font-size: 11px; color: var(--text-tertiary); font-family: monospace; margin-top: 1px; }

.status-light {
  display: inline-block;
  width: 8px; height: 8px;
  border-radius: 50%;
  margin-right: 6px;
}
.status-light.healthy { background: var(--color-success); box-shadow: 0 0 4px rgba(34,197,94,0.4); }
.status-light.degraded { background: var(--color-warning); }
.status-light.unhealthy { background: var(--text-muted); }
.status-light.unknown { background: var(--text-tertiary); }

.bar-cell { display: flex; align-items: center; gap: 8px; min-width: 80px; }
.bar-bg { flex: 1; height: 6px; border-radius: 3px; background: var(--bg-surface); overflow: hidden; }
.bar-fill { height: 100%; border-radius: 3px; }
.bar-fill.good { background: var(--color-success); }
.bar-fill.warn { background: var(--color-warning); }
.bar-text { font-size: 12px; font-weight: 600; color: var(--text-secondary); font-family: monospace; }
.time-cell { font-size: 12px; color: var(--text-secondary); }
.fw-600 { font-weight: 600; }

/* Budget */
.budget-cards { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
.budget-global {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px 16px;
  margin-bottom: 12px;
  background: var(--bg-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: var(--radius-md);
  font-size: 13px;
  color: var(--text-secondary);
}
.budget-global-values { display: inline-flex; gap: 8px; align-items: center; }
.budget-card { padding: 20px; background: var(--bg-surface); border-radius: var(--radius-md); display: flex; align-items: center; gap: 24px; border: 1px solid var(--border-subtle); }
.budget-ring { position: relative; flex-shrink: 0; }
.ring-text { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; }
.ring-value { font-size: 16px; font-weight: 700; color: var(--brand-primary); }
.ring-label { font-size: 10px; color: var(--text-secondary); }
.budget-details { display: flex; flex-direction: column; gap: 8px; flex: 1; }
.budget-details.full-width { width: 100%; }
.budget-row { display: flex; justify-content: space-between; font-size: 13px; color: var(--text-secondary); }
.budget-row.muted { color: var(--text-tertiary); font-size: 12px; }
.remaining { font-weight: 600; color: var(--color-success); }

/* Replay */
.replay-form { display: flex; gap: 12px; align-items: flex-end; flex-wrap: wrap; }
.input-group { display: flex; flex-direction: column; gap: 4px; }
.input-group label { font-size: 12px; color: var(--text-secondary); font-weight: 500; }
.text-input { padding: 8px 12px; border: 1px solid var(--border-default); border-radius: var(--radius-sm); font-size: 14px; outline: none; font-family: inherit; background: var(--bg-surface); color: var(--text-primary); }
.text-input:focus { border-color: var(--brand-primary); box-shadow: 0 0 0 2px rgba(99,102,241,0.1); }
.text-input.mono { font-family: monospace; }
.text-input.flex-1 { flex: 1; }
.btn-primary { padding: 8px 18px; border: none; border-radius: var(--radius-sm); background: var(--brand-primary); color: #fff; font-size: 14px; font-weight: 600; cursor: pointer; }
.btn-primary:hover { background: var(--brand-secondary); }
.btn-primary:disabled { opacity: 0.5; cursor: not-allowed; }
.replay-result { margin-top: 16px; }
.replay-header { font-size: 13px; font-weight: 600; color: var(--text-primary); margin-bottom: 8px; }
.replay-json { padding: 12px; background: var(--bg-primary); color: var(--text-primary); border-radius: var(--radius-sm); font-size: 13px; overflow-x: auto; white-space: pre-wrap; border: 1px solid var(--border-default); }

/* Responsive */
@media (max-width: 1024px) {
  .stats-grid {
    grid-template-columns: repeat(2, 1fr);
  }
  .budget-cards {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 768px) {
  .admin-header {
    flex-direction: column;
    align-items: flex-start;
    gap: 10px;
  }

  .tab-bar {
    overflow-x: auto;
    padding: 0 16px;
    -webkit-overflow-scrolling: touch;
  }

  .tab {
    white-space: nowrap;
    padding: 12px 14px;
    font-size: 12px;
  }

  .admin-content {
    padding: 16px;
  }

  .stats-grid {
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }

  .stat-card {
    padding: 12px 14px;
    gap: 10px;
  }

  .stat-icon {
    width: 36px;
    height: 36px;
  }

  .stat-value {
    font-size: 18px;
  }

  .budget-cards {
    grid-template-columns: 1fr;
  }

  .replay-form {
    flex-direction: column;
    align-items: stretch;
  }
}

@media (max-width: 480px) {
  .stats-grid {
    grid-template-columns: 1fr;
  }

  .admin-content {
    padding: 12px;
  }

  .section {
    padding: 14px;
  }
}
</style>
