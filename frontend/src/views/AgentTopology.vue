<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, inject, watch } from 'vue'
import { Icon } from '@iconify/vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { GraphChart } from 'echarts/charts'
import { TooltipComponent, LegendComponent } from 'echarts/components'
import AgentDetailPanel from '../components/agents/AgentDetailPanel.vue'
import { getAgents, getAgentMetrics } from '../services/grpc-client'
import { useChatStore } from '../stores/chat'
import type { AgentMetrics, ServiceInfo } from '../types/proto'

use([CanvasRenderer, GraphChart, TooltipComponent, LegendComponent])

// Chat Store
const chatStore = useChatStore()

const prefersReducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches

// Toast
const toast = inject<any>('toast')

// Types
interface AgentNode {
  id: string
  name: string
  type: 'orchestrator' | 'worker'
  status: 'HEALTHY' | 'DEGRADED' | 'DOWN'
  skills: string[]
  metrics: {
    success_rate: number
    avg_latency_ms: number
    total_requests: number
  }
}

interface Connection {
  source: string
  target: string
  status: 'active' | 'idle'
}

// State
// TODO: reuse stores/agents.ts instead of the local agents ref + 30s polling;
// deferred because this page uses a different data model (topology/connections)
// and custom ECharts rendering (refactor > 50 lines).
const loading = ref(true)
const agents = ref<AgentNode[]>([])
const connections = ref<Connection[]>([])
const selectedAgent = ref<AgentNode | null>(null)
const searchQuery = ref('')
const isFallback = ref(false)
let refreshTimer: ReturnType<typeof setInterval> | null = null

// Active Agent State (real-time topology)
const activeAgents = ref<Set<string>>(new Set())
const activeTimers = new Map<string, ReturnType<typeof setTimeout>>()
// Single all-agent reset timer for terminal entries (deep-review: the reset
// used to spawn one unregistered 5s timeout per complete/error entry — they
// stacked under rapid traffic and kept firing after unmount).
let activeResetTimer: ReturnType<typeof setTimeout> | null = null

// Computed
const filteredAgents = computed(() => {
  if (!searchQuery.value) return agents.value
  const q = searchQuery.value.toLowerCase()
  return agents.value.filter(a =>
    a.name.toLowerCase().includes(q) ||
    a.skills.some(s => s.toLowerCase().includes(q))
  )
})

const filteredAgentIds = computed(() => new Set(filteredAgents.value.map(a => a.id)))

const stats = computed(() => {
  const list = agents.value
  if (!list.length) return { online: 0, total: 0, avgSuccess: 0, avgLatency: 0, totalRequests: 0 }
  const online = list.filter(a => a.status !== 'DOWN').length
  const avgSuccess = list.reduce((s, a) => s + a.metrics.success_rate, 0) / list.length
  const avgLatency = Math.round(list.reduce((s, a) => s + a.metrics.avg_latency_ms, 0) / list.length)
  const totalRequests = list.reduce((s, a) => s + a.metrics.total_requests, 0)
  return { online, total: list.length, avgSuccess, avgLatency, totalRequests }
})



const chartOption = computed(() => {
  const hasActive = activeAgents.value.size > 0

  const nodes = filteredAgents.value.map(agent => {
    const isOrchestrator = agent.type === 'orchestrator'
    const baseSymbolSize = isOrchestrator ? 70 : 40 + agent.metrics.success_rate * 20
    const isActive = activeAgents.value.has(agent.id)
    const symbolSize = isActive ? baseSymbolSize * 1.2 : baseSymbolSize
    const statusColorMap: Record<string, string> = {
      HEALTHY: '#22c55e',
      DEGRADED: '#f59e0b',
      DOWN: '#ef4444',
    }
    const baseColor = statusColorMap[agent.status] || '#64748b'

    const itemStyle: any = {
      color: baseColor,
      borderColor: isActive ? '#ffffff' : (isOrchestrator ? '#818cf8' : baseColor),
      borderWidth: isActive ? 3 : (isOrchestrator ? 3 : 2),
      shadowBlur: isActive ? 60 : (prefersReducedMotion ? 0 : 10),
      shadowColor: isActive ? 'rgba(129, 140, 248, 0.8)' : 'rgba(0, 0, 0, 0.3)',
      shadowOffsetX: 0,
      shadowOffsetY: 4,
      opacity: isOrchestrator ? 1 : 0.92,
    }
    const labelStyle: any = {
      show: true,
      position: 'inside',
      formatter: agent.name,
      fontSize: isOrchestrator ? 13 : 11,
      fontWeight: isOrchestrator ? 'bold' : 'normal',
      color: '#ffffff',
      textBorderColor: 'rgba(0, 0, 0, 0.4)',
      textBorderWidth: 2,
    }

    // Dim non-active nodes when there are active ones
    if (hasActive && !isActive) {
      itemStyle.opacity = 0.5
      labelStyle.opacity = 0.5
    }

    return {
      id: agent.id,
      name: agent.name,
      symbolSize,
      symbol: 'circle',
      itemStyle,
      label: labelStyle,
      value: agent.metrics.success_rate,
    } as any
  })

  const links = connections.value
    .filter(c => filteredAgentIds.value.has(c.source) && filteredAgentIds.value.has(c.target))
    .map(conn => {
      const isEndpointActive = activeAgents.value.has(conn.source) || activeAgents.value.has(conn.target)
      const lineStyle: any = {
        color: isEndpointActive
          ? 'rgba(129, 140, 248, 0.9)'
          : conn.status === 'active'
            ? 'rgba(129, 140, 248, 0.8)'
            : 'rgba(100, 116, 139, 0.3)',
        width: isEndpointActive ? 3 : (conn.status === 'active' ? 2.5 : 1),
        type: conn.status === 'active' ? [10, 5] : [4, 8],
        curveness: 0.2,
        animationDuration: conn.status === 'active' ? 2000 : 5000,
      }
      return {
        source: conn.source,
        target: conn.target,
        lineStyle,
        symbol: ['none', 'arrow'],
        symbolSize: 8,
      }
    })

  return {
    backgroundColor: 'transparent',
    tooltip: {
      trigger: 'item',
      backgroundColor: 'rgba(17, 24, 39, 0.95)',
      borderColor: 'rgba(255, 255, 255, 0.1)',
      textStyle: { color: '#e2e8f0', fontSize: 12 },
      formatter: (params: any) => {
        if (params.dataType === 'node') {
          const agent = agents.value.find(a => a.id === params.data.id)
          if (!agent) return params.name
          const statusColors: Record<string, string> = { HEALTHY: '#22c55e', DEGRADED: '#f59e0b', DOWN: '#ef4444' }
          const statusLabels: Record<string, string> = { HEALTHY: '健康', DEGRADED: '降级', DOWN: '离线' }
          const sc = statusColors[agent.status] || '#64748b'
          const sl = statusLabels[agent.status] || agent.status
          const successPct = (agent.metrics.success_rate * 100).toFixed(1)
          const skillsHtml = agent.skills.map(s =>
            `<span style="display:inline-block;padding:2px 8px;margin:2px;font-size:10px;background:rgba(99,102,241,0.15);border:1px solid rgba(99,102,241,0.3);border-radius:9999px;color:#a5b4fc;">${s}</span>`
          ).join('')
          return `<div style="min-width:180px;">
            <div style="font-weight:700;font-size:14px;margin-bottom:6px;">${agent.name}</div>
            <div style="display:flex;align-items:center;gap:6px;margin-bottom:8px;">
              <span style="color:${sc};font-size:14px;">●</span>
              <span style="color:${sc};font-size:12px;font-weight:500;">${sl}</span>
            </div>
            <div style="margin-bottom:8px;">${skillsHtml}</div>
            <div style="font-size:11px;color:#94a3b8;margin-bottom:4px;">成功率</div>
            <div style="height:6px;background:rgba(255,255,255,0.08);border-radius:9999px;overflow:hidden;margin-bottom:6px;">
              <div style="height:100%;width:${successPct}%;background:linear-gradient(90deg,#6366f1,#a855f7);border-radius:9999px;"></div>
            </div>
            <div style="display:flex;justify-content:space-between;font-size:11px;color:#94a3b8;">
              <span>${successPct}%</span>
              <span>${agent.metrics.avg_latency_ms}ms</span>
              <span>${agent.metrics.total_requests.toLocaleString()} req</span>
            </div>
          </div>`
        }
        return ''
      },
    },
    series: [{
      type: 'graph',
      layout: 'force',
      roam: true,
      draggable: true,
      data: nodes,
      links,
      force: {
        repulsion: 500,
        edgeLength: [150, 250],
        gravity: 0.08,
        friction: 0.6,
      },
      emphasis: {
        focus: 'adjacency',
        scale: true,
        lineStyle: { width: 3, color: '#818cf8' },
        itemStyle: {
          shadowBlur: prefersReducedMotion ? 0 : 40,
          shadowColor: 'rgba(99, 102, 241, 0.6)',
          borderColor: '#818cf8',
          borderWidth: 4,
        },
        label: {
          fontSize: 14,
          fontWeight: 'bold',
          color: '#ffffff',
        },
      },
      animation: !prefersReducedMotion,
      animationDuration: 1500,
      animationEasing: 'linear',
      animationDurationUpdate: 800,
      animationEasingUpdate: 'cubicInOut',
    }],
  }
})

// Watch activityEntries for real-time topology
watch(() => chatStore.activityEntries.length, (newLen, oldLen) => {
  if (!oldLen || newLen <= oldLen) return
  const newEntries = chatStore.activityEntries.slice(oldLen)
  for (const entry of newEntries) {
    if (entry.type === 'agent_call' && entry.message.startsWith('Routed to Agent:')) {
      const agentName = entry.message.replace('Routed to Agent: ', '').trim()
      markAgentActive(agentName)
    }
    if (entry.type === 'tool_call' && entry.agent_name) {
      markAgentActive(entry.agent_name)
    }
    if (entry.type === 'complete' || entry.type === 'error') {
      if (activeResetTimer) clearTimeout(activeResetTimer)
      activeResetTimer = setTimeout(() => {
        activeAgents.value = new Set()
        activeResetTimer = null
      }, 5000)
    }
  }
})

function markAgentActive(name: string) {
  const matched = agents.value.find(a =>
    a.id.toLowerCase().includes(name.toLowerCase()) ||
    a.name.toLowerCase().includes(name.toLowerCase())
  )
  const id = matched?.id || name
  activeAgents.value = new Set([...activeAgents.value, id])
  if (activeTimers.has(id)) clearTimeout(activeTimers.get(id))
  activeTimers.set(id, setTimeout(() => {
    const next = new Set(activeAgents.value)
    next.delete(id)
    activeAgents.value = next
    activeTimers.delete(id)
  }, 30000))
}

// Methods
function onChartClick(params: any) {
  if (params.dataType === 'node') {
    const agent = agents.value.find(a => a.id === params.data.id)
    if (agent) {
      selectedAgent.value = agent
    }
  }
}

// Helper functions
function inferAgentType(agent: ServiceInfo): 'orchestrator' | 'worker' {
  if (agent.agent_card) {
    try {
      const card = JSON.parse(agent.agent_card)
      if (card.type === 'orchestrator') return 'orchestrator'
    } catch { /* ignore parse error */ }
  }
  if (agent.service_name?.toLowerCase().includes('orchestrator')) {
    return 'orchestrator'
  }
  return 'worker'
}

function inferStatus(metrics: AgentMetrics | null): 'HEALTHY' | 'DEGRADED' | 'DOWN' {
  if (!metrics) return 'DOWN'
  if (metrics.success_rate >= 0.8) return 'HEALTHY'
  if (metrics.success_rate >= 0.5) return 'DEGRADED'
  return 'DOWN'
}

function parseSkills(agent: ServiceInfo): string[] {
  if (agent.agent_card) {
    try {
      const card = JSON.parse(agent.agent_card)
      if (Array.isArray(card.skills)) {
        return card.skills.map((s: any) => typeof s === 'string' ? s : s.name || '')
          .filter(Boolean)
      }
      if (Array.isArray(card.capabilities)) return card.capabilities
    } catch { /* ignore */ }
  }
  return agent.skills || []
}

function inferConnections(agentList: AgentNode[]): Connection[] {
  const orchestrator = agentList.find(a => a.type === 'orchestrator')
  if (!orchestrator) return []
  return agentList
    .filter(a => a.type !== 'orchestrator')
    .map(a => ({
      source: orchestrator.id,
      target: a.id,
      status: (a.status !== 'DOWN' ? 'active' : 'idle') as 'active' | 'idle',
    }))
}

function useFallbackData() {
  agents.value = []
  connections.value = []
  isFallback.value = true
}

// Data loading
async function loadData() {
  loading.value = true
  try {
    // 1. Fetch all agents
    const agentsResponse = await getAgents()
    const agentList = agentsResponse.agents || []

    if (agentList.length === 0) {
      agents.value = []
      connections.value = []
      toast?.addToast({ type: 'warning', message: '未获取到Agent列表' })
      return
    }

    // 2. Fetch metrics for each agent concurrently
    // (keyed by the server registry id "service_name-host-port")
    const metricsPromises = agentList.map(a =>
      getAgentMetrics(`${a.service_name}-${a.host}-${a.port}`)
    )
    const metricsResults = await Promise.all(metricsPromises)

    // 3. Build topology nodes
    const nodes: AgentNode[] = agentList.map((agent, i) => {
      const metrics = metricsResults[i].data
      return {
        id: agent.service_name,
        name: agent.service_name,
        type: inferAgentType(agent),
        status: inferStatus(metrics),
        skills: parseSkills(agent),
        metrics: metrics
          ? {
              success_rate: metrics.success_rate,
              avg_latency_ms: metrics.avg_latency_ms,
              total_requests: metrics.total_requests,
            }
          : { success_rate: 0, avg_latency_ms: 0, total_requests: 0 },
      }
    })

    agents.value = nodes
    connections.value = inferConnections(nodes)
    isFallback.value = false
  } catch (e) {
    console.warn('Failed to load agent data', e)
    toast?.addToast({ type: 'error', message: '后端服务连接失败' })
    useFallbackData()
  } finally {
    loading.value = false
  }
}

async function refreshData() {
  await loadData()
}

// Lifecycle
onMounted(() => {
  loadData()
  // Auto-refresh every 30 seconds
  refreshTimer = setInterval(loadData, 30000)
})

onUnmounted(() => {
  if (refreshTimer) {
    clearInterval(refreshTimer)
    refreshTimer = null
  }
  // Clean up active agent timers
  for (const timer of activeTimers.values()) {
    clearTimeout(timer)
  }
  activeTimers.clear()
  if (activeResetTimer) {
    clearTimeout(activeResetTimer)
    activeResetTimer = null
  }
})
</script>

<template>
  <div class="topology-page">
    <!-- Fallback banner -->
    <div v-if="isFallback" class="fallback-banner">
      <Icon icon="mdi:alert-circle-outline" :width="18" />
      <span>后端服务未连接，拓扑图暂无数据</span>
    </div>

    <!-- Header -->
    <div class="page-header">
      <div class="header-left">
        <h1 class="page-title gradient-text">Agent 网络拓扑</h1>
        <div v-if="chatStore.isStreaming" class="live-badge">
          <span class="live-dot"></span>
          LIVE
        </div>
      </div>
      <div class="toolbar">
        <div class="search-box">
          <Icon icon="mdi:magnify" :width="18" class="search-icon" />
          <input
            v-model="searchQuery"
            type="text"
            placeholder="搜索 Agent..."
            class="search-input"
          />
        </div>
        <button class="refresh-btn" @click="refreshData" :disabled="loading">
          <Icon icon="mdi:refresh" :width="20" :class="{ spinning: loading }" />
        </button>
      </div>
    </div>

    <!-- Stats summary bar -->
    <div class="stats-bar">
      <div class="stat-card">
        <div class="stat-icon stat-icon--online">
          <Icon icon="mdi:account-check" :width="20" />
        </div>
        <div class="stat-info">
          <div class="stat-value">{{ stats.online }}<span class="stat-total"> / {{ stats.total }}</span></div>
          <div class="stat-label">在线 Agent</div>
        </div>
      </div>
      <div class="stat-card">
        <div class="stat-icon stat-icon--success">
          <Icon icon="mdi:check-circle" :width="20" />
        </div>
        <div class="stat-info">
          <div class="stat-value">{{ (stats.avgSuccess * 100).toFixed(1) }}<span class="stat-unit">%</span></div>
          <div class="stat-label">平均成功率</div>
        </div>
      </div>
      <div class="stat-card">
        <div class="stat-icon stat-icon--latency">
          <Icon icon="mdi:timer-outline" :width="20" />
        </div>
        <div class="stat-info">
          <div class="stat-value">{{ stats.avgLatency }}<span class="stat-unit">ms</span></div>
          <div class="stat-label">平均延迟</div>
        </div>
      </div>
      <div class="stat-card">
        <div class="stat-icon stat-icon--requests">
          <Icon icon="mdi:chart-bar" :width="20" />
        </div>
        <div class="stat-info">
          <div class="stat-value">{{ stats.totalRequests.toLocaleString() }}</div>
          <div class="stat-label">总请求数</div>
        </div>
      </div>
    </div>

    <!-- Main content -->
    <div class="main-content">
      <!-- Chart area -->
      <div class="chart-area">
        <!-- Loading skeleton -->
        <div v-if="loading" class="skeleton-container">
          <div class="skeleton-graph">
            <div class="skeleton-node skeleton-pulse" style="width: 70px; height: 70px; border-radius: 50%;"></div>
            <div class="skeleton-lines">
              <div class="skeleton-line skeleton-pulse" style="width: 120px;"></div>
              <div class="skeleton-line skeleton-pulse" style="width: 100px;"></div>
              <div class="skeleton-line skeleton-pulse" style="width: 140px;"></div>
            </div>
            <div class="skeleton-nodes-row">
              <div class="skeleton-node skeleton-pulse" style="width: 50px; height: 50px; border-radius: 50%;"></div>
              <div class="skeleton-node skeleton-pulse" style="width: 50px; height: 50px; border-radius: 50%;"></div>
              <div class="skeleton-node skeleton-pulse" style="width: 50px; height: 50px; border-radius: 50%;"></div>
            </div>
          </div>
        </div>

        <!-- ECharts graph -->
        <VChart
          v-else
          :option="chartOption"
          class="topology-chart"
          @click="onChartClick"
          autoresize
        />
      </div>

      <!-- Detail panel -->
      <AgentDetailPanel
        :agent="selectedAgent"
        :active="selectedAgent ? activeAgents.has(selectedAgent.id) : false"
      />
    </div>
  </div>
</template>

<style scoped>
@import "../styles/design-tokens.css";

.topology-page {
  display: flex;
  flex-direction: column;
  height: 100%;
  padding: var(--space-6);
  overflow: hidden;
}

.fallback-banner {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-3) var(--space-4);
  margin-bottom: var(--space-4);
  background: rgba(245, 158, 11, 0.1);
  border: 1px solid rgba(245, 158, 11, 0.3);
  border-radius: var(--radius-md);
  color: #f59e0b;
  font-size: 13px;
  flex-shrink: 0;
}

.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: var(--space-5);
  flex-shrink: 0;
}

.header-left {
  display: flex;
  align-items: center;
  gap: var(--space-4);
}

.page-title {
  font-size: 24px;
  font-weight: 800;
  margin: 0;
}

/* LIVE badge */
.live-badge {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 12px;
  background: rgba(239, 68, 68, 0.12);
  border: 1px solid rgba(239, 68, 68, 0.3);
  border-radius: var(--radius-full);
  color: #ef4444;
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 1px;
  animation: live-fade 2s ease-in-out infinite;
}

.live-dot {
  width: 8px;
  height: 8px;
  border-radius: var(--radius-full);
  background: #ef4444;
  animation: live-pulse 1.5s ease-in-out infinite;
}

@keyframes live-pulse {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.5; transform: scale(0.8); }
}

@keyframes live-fade {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.7; }
}

.toolbar {
  display: flex;
  align-items: center;
  gap: var(--space-3);
}

.search-box {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-2) var(--space-3);
  background: var(--glass-bg);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-md);
  transition: all var(--duration-normal) var(--ease-default);
}

.search-box:focus-within {
  border-color: var(--border-brand);
  box-shadow: var(--shadow-glow-brand);
}

.search-icon {
  color: var(--text-tertiary);
}

.search-input {
  background: transparent;
  border: none;
  outline: none;
  color: var(--text-primary);
  font-size: 13px;
  width: 160px;
  font-family: var(--font-sans);
}

.search-input::placeholder {
  color: var(--text-tertiary);
}

.refresh-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-md);
  background: var(--glass-bg);
  color: var(--text-secondary);
  cursor: pointer;
  transition: all var(--duration-normal) var(--ease-default);
}

.refresh-btn:hover:not(:disabled) {
  background: var(--glass-bg-hover);
  border-color: var(--border-brand);
  color: var(--brand-primary);
}

.refresh-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.spinning {
  animation: spin 1s linear infinite;
}

@keyframes spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}

/* Stats summary bar */
.stats-bar {
  display: flex;
  gap: var(--space-3);
  margin-bottom: var(--space-4);
  flex-shrink: 0;
  overflow-x: auto;
  scrollbar-width: none;
}

.stats-bar::-webkit-scrollbar {
  display: none;
}

.stat-card {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: var(--space-3) var(--space-4);
  background: var(--glass-bg);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-md);
  min-width: 140px;
  flex-shrink: 0;
  transition: all var(--duration-normal) var(--ease-default);
}

.stat-card:hover {
  background: var(--glass-bg-hover);
  border-color: var(--border-brand);
}

.stat-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border-radius: var(--radius-md);
  flex-shrink: 0;
}

.stat-icon--online {
  background: rgba(34, 197, 94, 0.12);
  color: #22c55e;
}

.stat-icon--success {
  background: rgba(99, 102, 241, 0.12);
  color: #818cf8;
}

.stat-icon--latency {
  background: rgba(245, 158, 11, 0.12);
  color: #f59e0b;
}

.stat-icon--requests {
  background: rgba(168, 85, 247, 0.12);
  color: #a855f7;
}

.stat-info {
  display: flex;
  flex-direction: column;
}

.stat-value {
  font-size: 20px;
  font-weight: 700;
  color: var(--text-primary);
  font-family: var(--font-mono);
  line-height: 1.2;
}

.stat-unit {
  font-size: 13px;
  font-weight: 500;
  color: var(--text-tertiary);
  margin-left: 2px;
}

.stat-total {
  font-size: 13px;
  font-weight: 400;
  color: var(--text-tertiary);
}

.stat-label {
  font-size: 11px;
  color: var(--text-tertiary);
  text-transform: uppercase;
  letter-spacing: 0.5px;
  margin-top: 2px;
}

.main-content {
  flex: 1;
  display: flex;
  gap: var(--space-5);
  min-height: 0;
}

.chart-area {
  flex: 1;
  min-width: 0;
  min-height: 400px;
  background: var(--glass-bg);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-lg);
  position: relative;
  overflow: hidden;
}

.topology-chart {
  width: 100%;
  height: 100%;
}

/* Skeleton loading */
.skeleton-container {
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100%;
}

.skeleton-graph {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--space-6);
}

.skeleton-nodes-row {
  display: flex;
  gap: var(--space-8);
}

.skeleton-lines {
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
  align-items: center;
}

.skeleton-line {
  height: 3px;
  background: rgba(255, 255, 255, 0.06);
  border-radius: var(--radius-full);
}

.skeleton-node {
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid rgba(255, 255, 255, 0.08);
}

.skeleton-pulse {
  animation: skeleton-pulse 1.5s ease-in-out infinite;
}

@keyframes skeleton-pulse {
  0%, 100% { opacity: 0.4; }
  50% { opacity: 1; }
}

/* Responsive */
@media (max-width: 1024px) {
  .main-content {
    flex-direction: column;
  }

  .topology-page {
    padding: var(--space-4);
  }
}

@media (max-width: 768px) {
  .stat-card {
    min-width: 120px;
    padding: var(--space-2) var(--space-3);
  }
  .stat-value {
    font-size: 16px;
  }
  .page-header {
    flex-direction: column;
    align-items: flex-start;
    gap: var(--space-3);
  }

  .header-left {
    flex-wrap: wrap;
  }

  .toolbar {
    width: 100%;
    flex-wrap: wrap;
  }

  .search-input {
    width: 120px;
  }

  .page-title {
    font-size: 20px;
  }

  .main-content {
    flex-direction: column;
  }

  .chart-area {
    min-height: 300px;
  }
}

@media (max-width: 480px) {
  .topology-page {
    padding: var(--space-3);
  }

  .search-input {
    width: 100px;
  }
}

/* Reduced motion */
@media (prefers-reduced-motion: reduce) {
  .spinning {
    animation: none;
  }

  .skeleton-pulse {
    animation: none;
    opacity: 0.6;
  }

  .live-dot {
    animation: none;
  }

  .live-badge {
    animation: none;
  }
}
</style>
