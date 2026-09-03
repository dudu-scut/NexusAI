<template>
  <div class="execution-plan" v-if="plan">
    <div class="plan-header">
      <span class="plan-icon">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
          <rect x="3" y="3" width="18" height="18" rx="2" ry="2"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="9" y1="21" x2="9" y2="9"/>
        </svg>
      </span>
      <span class="plan-title">Execution Plan</span>
      <span class="plan-badge">{{ completedCount }}/{{ plan.tasks.length }}</span>
    </div>

    <!-- DAG Visual -->
    <div v-if="showDag" class="dag-container">
      <div ref="dagRef" class="mermaid-container"></div>
    </div>

    <button
      v-if="plan.tasks.length > 1"
      class="dag-toggle"
      @click="toggleDag"
    >
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/><line x1="12" y1="22" x2="12" y2="15.5"/><polyline points="22 8.5 12 15.5 2 8.5"/>
      </svg>
      {{ showDag ? 'Hide Flowchart' : 'Show Flowchart' }}
    </button>

    <!-- Task Cards -->
    <div class="plan-tasks">
      <div
        v-for="task in plan.tasks"
        :key="task.id"
        class="task-card"
        :class="task.status"
      >
        <div class="task-header">
          <span class="task-status-icon">
            <template v-if="task.status === 'pending'">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/></svg>
            </template>
            <template v-else-if="task.status === 'running'">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="spin"><path d="M21 12a9 9 0 1 1-6.219-8.56"/></svg>
            </template>
            <template v-else-if="task.status === 'completed'">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="20 6 9 17 4 12"/></svg>
            </template>
            <template v-else-if="task.status === 'failed'">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
            </template>
          </span>
          <span class="task-id">{{ task.id }}</span>
          <span class="task-skill">{{ task.skill }}</span>
          <span v-if="task.agent_id" class="task-agent">{{ task.agent_name || task.agent_id }}</span>
        </div>

        <div class="task-description">{{ task.description }}</div>

        <div v-if="task.depends_on.length > 0" class="task-deps">
          <span v-for="dep in task.depends_on" :key="dep" class="dep-tag">
            ← {{ dep }}
          </span>
        </div>

        <div v-if="task.result && task.status !== 'pending'" class="task-result">
          {{ truncate(task.result, 200) }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, nextTick } from 'vue'
import type { ExecutionPlan } from '../types/proto'

const props = defineProps<{ plan: ExecutionPlan }>()

const showDag = ref(false)
const dagRef = ref<HTMLDivElement>()

const completedCount = computed(() =>
  props.plan.tasks.filter(t => t.status === 'completed' || t.status === 'failed').length
)

function generateMermaid(plan: ExecutionPlan): string {
  let mermaid = 'graph TD\n'
  mermaid += '  Q["🔍 Query"] --> N1\n'

  plan.tasks.forEach((task, i) => {
    const nodeId = `N${i + 1}`
    const icon = task.status === 'completed' ? '✅' :
                 task.status === 'running' ? '⏳' :
                 task.status === 'failed' ? '❌' : '○'
    mermaid += `  ${nodeId}["${icon} ${task.skill}\\n${task.description.slice(0, 30)}"]\n`

    for (const dep of task.depends_on) {
      const depIdx = plan.tasks.findIndex(t => t.id === dep)
      if (depIdx >= 0) {
        mermaid += `  N${depIdx + 1} --> ${nodeId}\n`
      }
    }
  })

  // Style — dark theme DAG node colors
  mermaid += '  classDef done fill:rgba(16,185,129,0.15),stroke:#10b981,color:#f1f5f9\n'
  mermaid += '  classDef running fill:rgba(59,130,246,0.15),stroke:#3b82f6,color:#f1f5f9\n'
  mermaid += '  classDef failed fill:rgba(239,68,68,0.15),stroke:#ef4444,color:#f1f5f9\n'

  plan.tasks.forEach((task, i) => {
    if (task.status === 'completed') mermaid += `  class N${i + 1} done\n`
    if (task.status === 'running') mermaid += `  class N${i + 1} running\n`
    if (task.status === 'failed') mermaid += `  class N${i + 1} failed\n`
  })

  return mermaid
}

async function toggleDag() {
  showDag.value = !showDag.value
  if (showDag.value) {
    await nextTick()
    await renderMermaid()
  }
}

async function renderMermaid() {
  if (!dagRef.value) return
  try {
    const mermaid = await import('mermaid')
    mermaid.default.initialize({
      startOnLoad: false,
      theme: 'dark',
      themeVariables: {
        darkMode: true,
        background: '#111827',
        primaryColor: '#6366f1',
        primaryTextColor: '#e2e8f0',
        primaryBorderColor: '#818cf8',
        lineColor: '#818cf8',
        secondaryColor: '#1e293b',
        tertiaryColor: '#0f172a',
      },
      securityLevel: 'loose',
      flowchart: {
        useMaxWidth: true,
        htmlLabels: true,
        curve: 'basis',
      },
    })
    const { svg } = await mermaid.default.render(
      `dag-${Date.now()}`,
      generateMermaid(props.plan)
    )
    dagRef.value.innerHTML = svg
  } catch {
    dagRef.value.innerHTML = '<p style="color:var(--text-tertiary);font-size:12px;text-align:center">Flowchart render failed</p>'
  }
}

function truncate(text: string, max: number): string {
  if (!text) return ''
  return text.length > max ? text.slice(0, max) + '...' : text
}
</script>

<style scoped>
@import "../styles/design-tokens.css";

.execution-plan {
  margin: var(--space-2) 0 var(--space-3);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-md);
  overflow: hidden;
  background: var(--glass-bg);
  backdrop-filter: blur(var(--glass-blur));
}

.plan-header {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-3) var(--space-4);
  background: var(--bg-elevated);
  border-bottom: 1px solid var(--border-subtle);
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.plan-icon {
  display: flex;
  align-items: center;
  color: var(--brand-primary);
}

.plan-badge {
  margin-left: auto;
  padding: 2px 8px;
  border-radius: var(--radius-full);
  background: var(--bg-tertiary);
  font-size: 12px;
  font-weight: 500;
  color: var(--text-tertiary);
}

/* DAG */
.dag-toggle {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  width: 100%;
  padding: var(--space-2) var(--space-4);
  border: none;
  border-bottom: 1px solid var(--border-subtle);
  background: var(--bg-surface);
  color: var(--brand-primary);
  font-size: 12px;
  font-weight: 500;
  cursor: pointer;
  transition: background var(--duration-fast) var(--ease-default);
}

.dag-toggle:hover {
  background: var(--glass-bg-hover);
}

.dag-container {
  padding: var(--space-3);
  background: var(--bg-secondary);
  border-bottom: 1px solid var(--border-subtle);
}

.mermaid-container {
  display: flex;
  justify-content: center;
  overflow-x: auto;
}

.mermaid-container :deep(svg) {
  max-width: 100%;
  height: auto;
}

/* Tasks */
.plan-tasks {
  display: flex;
  flex-direction: column;
}

.task-card {
  padding: var(--space-3) var(--space-4);
  border-bottom: 1px solid var(--border-subtle);
  transition: background var(--duration-normal) var(--ease-default);
}

.task-card:last-child {
  border-bottom: none;
}

.task-card.running {
  background: rgba(59, 130, 246, 0.08);
}

.task-card.completed {
  background: rgba(16, 185, 129, 0.08);
}

.task-card.failed {
  background: rgba(239, 68, 68, 0.08);
}

.task-header {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  margin-bottom: var(--space-1);
  flex-wrap: wrap;
}

.task-status-icon {
  width: 18px;
  height: 18px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}

.task-card.pending .task-status-icon { color: var(--text-tertiary); }
.task-card.running .task-status-icon { color: var(--color-info); }
.task-card.completed .task-status-icon { color: var(--color-success); }
.task-card.failed .task-status-icon { color: var(--color-error); }

.spin {
  animation: spin 1s linear infinite;
}

@keyframes spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}

.task-id {
  font-size: 12px;
  font-weight: 600;
  color: var(--text-tertiary);
  font-family: var(--font-mono);
}

.task-skill {
  font-size: 11px;
  padding: 1px 6px;
  border-radius: var(--radius-sm);
  background: var(--bg-elevated);
  color: var(--text-secondary);
  border: 1px solid var(--border-subtle);
}

.task-agent {
  font-size: 11px;
  padding: 1px 6px;
  border-radius: var(--radius-sm);
  background: rgba(99, 102, 241, 0.12);
  color: var(--brand-primary);
  font-family: var(--font-mono);
  border: 1px solid var(--border-brand);
}

.task-description {
  font-size: 13px;
  color: var(--text-secondary);
  line-height: 1.4;
}

.task-deps {
  display: flex;
  gap: var(--space-1);
  margin-top: var(--space-1);
  flex-wrap: wrap;
}

.dep-tag {
  font-size: 11px;
  color: var(--text-muted);
}

.task-result {
  margin-top: var(--space-2);
  font-size: 12px;
  color: var(--text-tertiary);
  line-height: 1.4;
  padding: var(--space-2) var(--space-3);
  background: var(--bg-surface);
  border-radius: var(--radius-sm);
  border: 1px solid var(--border-subtle);
  white-space: pre-wrap;
  word-break: break-word;
}
</style>
