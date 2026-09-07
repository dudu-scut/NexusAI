<template>
  <div class="compare-view">
    <div class="compare-header">
      <router-link to="/" class="back-link">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
      </router-link>
      <h1>Agent Compare</h1>
      <span class="subtitle">Compare Agent responses side by side</span>
    </div>

    <div class="compare-controls">
      <div class="control-group">
        <label>Select Agents (up to 3)</label>
        <div class="agent-selects">
          <select v-model="selectedAgents[0]" class="select-input">
            <option value="">-- Select Agent --</option>
            <option v-for="a in availableAgents" :key="a.id" :value="a.id">{{ a.label }}</option>
          </select>
          <select v-model="selectedAgents[1]" class="select-input">
            <option value="">-- Select Agent --</option>
            <option v-for="a in availableAgents" :key="a.id" :value="a.id">{{ a.label }}</option>
          </select>
          <select v-model="selectedAgents[2]" class="select-input">
            <option value="">-- Select Agent --</option>
            <option v-for="a in availableAgents" :key="a.id" :value="a.id">{{ a.label }}</option>
          </select>
        </div>
      </div>
      <div class="query-group">
        <textarea v-model="compareQuery" placeholder="Enter comparison query..." class="query-input" rows="3"></textarea>
        <button class="btn-compare" :disabled="comparing || !compareQuery.trim()" @click="runCompare">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>
          {{ comparing ? 'Comparing…' : 'Compare' }}
        </button>
      </div>
    </div>

    <div class="results-area">
      <!-- Loading -->
      <GlassCard v-if="comparing" variant="highlight" padding="lg">
        <div class="compare-status">Running the same question through up to 3 agents in parallel…</div>
      </GlassCard>

      <!-- Error -->
      <GlassCard v-else-if="compareError" variant="highlight" padding="lg">
        <div class="compare-error">{{ compareError }}</div>
      </GlassCard>

      <!-- Result -->
      <template v-else-if="compareOutcome">
        <div class="run-summary">
          Run <code>{{ compareOutcome.run_id }}</code>
          <span class="run-status" :data-status="compareOutcome.run_status">{{ compareOutcome.run_status }}</span>
        </div>
        <div class="result-grid">
          <GlassCard v-for="result in compareOutcome.results" :key="result.agent_id" variant="highlight" padding="lg">
            <div class="result-card">
              <div class="result-head">
                <strong>{{ result.agent_id }}</strong>
                <span class="agent-status" :data-status="result.status">{{ result.status }}</span>
              </div>
              <pre v-if="result.status === 'completed'" class="agent-answer">{{ result.answer }}</pre>
              <div v-else class="agent-error">{{ result.error || 'No answer' }}</div>
              <div v-if="result.request_id" class="agent-meta">request_id: <code>{{ result.request_id }}</code></div>
            </div>
          </GlassCard>
        </div>
      </template>

      <!-- Empty -->
      <GlassCard v-else variant="highlight" padding="lg">
        <div class="compare-status">Select 1–3 agents and a question, then run a real comparison. Each agent answers independently; failures stay visible per agent.</div>
      </GlassCard>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { getAgents, compareAgents } from '../services/grpc-client'
import type { CompareAgentsResponse } from '../types/proto'
import GlassCard from '../components/layout/GlassCard.vue'

const availableAgents = ref<{ id: string; label: string }[]>([])
const selectedAgents = ref<string[]>(['', '', ''])
const compareQuery = ref('')

// Compare states: loading / error / result / empty
const comparing = ref(false)
const compareError = ref('')
const compareOutcome = ref<CompareAgentsResponse | null>(null)

onMounted(async () => {
  try {
    const resp = await getAgents()
    // deep-review fr-R1: the backend resolves agents by their COMPOSITE
    // registration key (service_name-host-port) — submitting the bare
    // service_name made every comparison fail with "agent is not registered".
    availableAgents.value = resp.agents.map(a => ({
      id: `${a.service_name}-${a.host}-${a.port}`,
      label: a.service_name,
    }))
  } catch (e) {
    console.warn('Failed to load agents:', e)
  }
})

async function runCompare() {
  const agentIds = selectedAgents.value.filter(a => a !== '')
  if (!compareQuery.value.trim() || agentIds.length === 0) return
  comparing.value = true
  compareError.value = ''
  compareOutcome.value = null
  try {
    compareOutcome.value = await compareAgents(compareQuery.value.trim(), agentIds)
  } catch (e) {
    compareError.value = e instanceof Error ? e.message : String(e)
  } finally {
    comparing.value = false
  }
}
</script>

<style scoped>
@import "../styles/design-tokens.css";

.compare-view { min-height: 100vh; background: var(--bg-primary); }

.compare-header {
  display: flex; align-items: center; gap: 12px;
  padding: 20px 32px; background: var(--bg-elevated); border-bottom: 1px solid var(--border-default);
}

.back-link { display: flex; color: var(--text-secondary); padding: 4px; border-radius: var(--radius-sm); transition: all 0.15s; }
.back-link:hover { background: var(--glass-bg-hover); }

.compare-header h1 { font-size: 22px; font-weight: 700; margin: 0; color: var(--text-primary); }
.subtitle { font-size: 13px; color: var(--text-tertiary); }

.compare-controls {
  padding: 24px 32px; background: var(--bg-elevated); border-bottom: 1px solid var(--border-default);
  display: flex; gap: 20px; flex-wrap: wrap; align-items: flex-end;
}

.control-group { display: flex; flex-direction: column; gap: 8px; }
.control-group label { font-size: 12px; color: var(--text-secondary); font-weight: 500; }

.agent-selects { display: flex; gap: 8px; }

.select-input {
  padding: 8px 12px; border: 1px solid var(--border-default); border-radius: var(--radius-sm);
  font-size: 13px; background: var(--bg-surface); color: var(--text-primary); outline: none; min-width: 160px;
}

.select-input:focus { border-color: var(--brand-primary); }

.query-group { display: flex; gap: 10px; align-items: flex-end; flex: 1; }

.query-input {
  flex: 1; padding: 10px 14px; border: 1px solid var(--border-default); border-radius: var(--radius-md);
  font-size: 14px; font-family: inherit; resize: vertical; outline: none;
  min-width: 200px; background: var(--bg-surface); color: var(--text-primary);
}

.query-input:focus { border-color: var(--brand-primary); box-shadow: 0 0 0 3px rgba(99,102,241,0.1); }

.btn-compare {
  display: flex; align-items: center; gap: 8px;
  padding: 10px 20px; border: none; border-radius: var(--radius-md);
  background: var(--brand-primary); color: #fff; font-size: 14px; font-weight: 600; cursor: pointer;
}

.btn-compare:disabled { opacity: 0.5; cursor: not-allowed; }

.results-area { padding: 24px 32px; max-width: 1400px; }

.compare-status { font-size: 13px; color: var(--text-secondary); line-height: 1.6; }
.compare-error { font-size: 13px; color: var(--status-error, #ef4444); }

.run-summary {
  font-size: 13px; color: var(--text-secondary); margin-bottom: 12px;
  display: flex; align-items: center; gap: 8px;
}
.run-summary code { font-size: 11px; color: var(--text-tertiary); }

.run-status, .agent-status {
  padding: 2px 10px; border-radius: 10px; font-size: 11px; font-weight: 600;
  border: 1px solid var(--border-subtle); color: var(--text-secondary);
}
.run-status[data-status="completed"], .agent-status[data-status="completed"] { color: #22c55e; border-color: #22c55e; }
.run-status[data-status="partial"], .agent-status[data-status="failed"] { color: var(--status-error, #ef4444); border-color: var(--status-error, #ef4444); }

.result-grid {
  display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 16px;
}

.result-card { display: flex; flex-direction: column; gap: 10px; }
.result-head { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
.result-head strong { font-size: 14px; color: var(--text-primary); }

.agent-answer {
  margin: 0; white-space: pre-wrap; word-break: break-word;
  font-size: 13px; color: var(--text-primary); max-height: 280px; overflow: auto;
}
.agent-error { font-size: 13px; color: var(--status-error, #ef4444); }
.agent-meta { font-size: 11px; color: var(--text-tertiary); }
.agent-meta code { font-size: 10px; }

/* Responsive */
@media (max-width: 1024px) {
  .compare-controls {
    padding: 20px 24px;
  }
  .results-area {
    padding: 20px 24px;
  }
}

@media (max-width: 768px) {
  .compare-header {
    padding: 16px 20px;
  }
  .compare-controls {
    flex-direction: column;
    padding: 16px 20px;
  }
  .agent-selects {
    flex-direction: column;
  }
  .select-input {
    min-width: auto;
    width: 100%;
  }
  .query-group {
    flex-direction: column;
  }
  .query-input {
    min-width: auto;
  }
  .results-area {
    padding: 16px 20px;
  }
}

@media (max-width: 480px) {
  .compare-header h1 {
    font-size: 18px;
  }
  .subtitle {
    display: none;
  }
}
</style>
