<template>
  <div class="monitor-page">
    <!-- Header -->
    <header class="monitor-header">
      <h1 class="gradient-text page-title">系统监控</h1>
      <div class="header-controls">
        <label class="auto-refresh-toggle">
          <span class="toggle-label">自动刷新</span>
          <button
            class="toggle-btn"
            :class="{ active: autoRefresh }"
            @click="autoRefresh = !autoRefresh"
          >
            <span class="toggle-knob" />
          </button>
        </label>
        <select v-model.number="refreshInterval" class="interval-select" :disabled="!autoRefresh">
          <option :value="5">5s</option>
          <option :value="10">10s</option>
          <option :value="30">30s</option>
          <option :value="60">60s</option>
        </select>
        <button class="refresh-btn" :class="{ spinning: refreshing }" @click="manualRefresh" aria-label="手动刷新数据">
          <Icon icon="mdi:refresh" width="18" height="18" aria-hidden="true" />
        </button>
      </div>
    </header>

    <!-- Connection Warning Banner -->
    <div v-if="!dataAvailable" class="connection-warning">
      <Icon icon="mdi:alert-circle-outline" width="18" height="18" />
      <span>后端服务不可用，当前显示回退数据。请检查 gRPC 服务连接。</span>
    </div>

    <!-- Row 1: 3 equal columns -->
    <div class="row row-3col">
      <!-- Health Overview -->
      <GlassCard class="panel">
        <h3 class="panel-title">系统健康概览</h3>
        <div class="gauge-wrap">
          <v-chart :option="healthGaugeOption" class="chart" autoresize />
        </div>
        <div class="health-stats">
          <div class="health-stat">
            <span class="dot dot-success" />
            <span class="stat-num">{{ healthOverview.healthy }}</span>
            <span class="stat-label">健康</span>
          </div>
          <div class="health-stat">
            <span class="dot dot-warning" />
            <span class="stat-num">{{ healthOverview.degraded }}</span>
            <span class="stat-label">降级</span>
          </div>
          <div class="health-stat">
            <span class="dot dot-error" />
            <span class="stat-num">{{ healthOverview.offline }}</span>
            <span class="stat-label">离线</span>
          </div>
        </div>
      </GlassCard>

      <!-- Latency Heatmap (from TraceSpan aggregation) -->
      <GlassCard class="panel">
        <h3 class="panel-title">请求延迟分布</h3>
        <div v-if="latencyAvailable" class="chart-container">
          <v-chart :option="latencyDistributionOption" class="chart" autoresize />
        </div>
        <EmptyState
          v-else
          icon="mdi:chart-timeline-variant"
          title="暂无延迟分布数据"
          description="延迟/错误采集面板暂未接入数据通道"
        />
      </GlassCard>

      <!-- Error Rate Trend (from TraceSpan status aggregation) -->
      <GlassCard class="panel">
        <h3 class="panel-title">错误率趋势</h3>
        <div v-if="errorRateAvailable" class="chart-container">
          <v-chart :option="errorRateOption" class="chart" autoresize />
        </div>
        <EmptyState
          v-else
          icon="mdi:chart-line-variant"
          title="暂无错误率趋势"
          description="延迟/错误采集面板暂未接入数据通道"
        />
      </GlassCard>
    </div>

    <!-- Row 2: 2/3 + 1/3 -->
    <div class="row row-2col">
      <!-- Circuit Breaker Panel -->
      <GlassCard class="panel cb-panel">
        <h3 class="panel-title">断路器状态</h3>
        <div v-if="circuitBreakers.length > 0" class="cb-grid">
          <div
            v-for="cb in circuitBreakers"
            :key="cb.agent"
            class="cb-card"
            :class="`cb-${cb.state.toLowerCase()}`"
          >
            <div class="cb-header">
              <span class="cb-indicator" :class="`indicator-${cb.state.toLowerCase()}`" />
              <span class="cb-agent">{{ cb.agent }}</span>
            </div>
            <div class="cb-state-label">{{ stateLabel(cb.state) }}</div>
            <div class="cb-details">
              <div class="cb-detail">
                <span class="cb-detail-label">失败次数</span>
                <span class="cb-detail-value">{{ cb.failures }}</span>
              </div>
              <div class="cb-detail">
                <span class="cb-detail-label">上次触发</span>
                <span class="cb-detail-value">{{ cb.lastTrigger || '—' }}</span>
              </div>
              <div class="cb-detail">
                <span class="cb-detail-label">恢复时间</span>
                <span class="cb-detail-value">{{ cb.recoveryTime || '—' }}</span>
              </div>
            </div>
          </div>
        </div>
        <EmptyState
          v-else
          icon="mdi:electric-switch"
          title="暂无断路器数据"
          description="断路器数据需要后端 Agent 指标 API 支持"
        />
      </GlassCard>

      <!-- Cache Hit Rate (needs Redis stats API) -->
      <GlassCard class="panel cache-panel">
        <h3 class="panel-title">缓存命中率</h3>
        <EmptyState
          icon="mdi:database-outline"
          title="暂无缓存数据"
          description="缓存统计需要后端 API 支持"
        />
      </GlassCard>
    </div>

    <!-- Row 3: Full-width Alert Stream (needs alerts RPC) -->
    <GlassCard class="panel alert-panel">
      <h3 class="panel-title">告警流</h3>
      <EmptyState
        icon="mdi:bell-outline"
        title="暂无告警数据"
        description="告警数据需要后端 alerts API 支持"
      />
    </GlassCard>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { GaugeChart } from 'echarts/charts'
import { GridComponent, TooltipComponent } from 'echarts/components'
import { BarChart, LineChart } from 'echarts/charts'
import GlassCard from '../components/layout/GlassCard.vue'
import EmptyState from '../components/feedback/EmptyState.vue'
import { Icon } from '@iconify/vue'
import { getAgents, getAgentMetrics } from '../services/grpc-client'

use([CanvasRenderer, GaugeChart, BarChart, LineChart, GridComponent, TooltipComponent])

// State
const dataAvailable = ref(false)

const healthOverview = ref({ score: 0, healthy: 0, degraded: 0, offline: 0 })

// Latency distribution data (from TraceSpan aggregation)
const latencyBuckets = ref<{ range: string; count: number }[]>([])
const latencyAvailable = ref(false)

// Error rate trend data (from TraceSpan status aggregation)
const errorRateData = ref<{ time: string; rate: number }[]>([])
const errorRateAvailable = ref(false)

const circuitBreakers = ref<Array<{
  agent: string
  state: string
  failures: number
  lastTrigger: string | null
  recoveryTime: string | null
}>>([])

// Fallback data (used when API unavailable)
function useFallbackData() {
  healthOverview.value = { score: 0, healthy: 0, degraded: 0, offline: 0 }
  circuitBreakers.value = []
}

// Load trace data for latency/error panels
// deep-review D-F1: the former source (ChatMessage.traceInfo, which no code
// path ever filled) was dead — the backend has no trace-list RPC, so there
// is no way to enumerate trace ids from the dashboard today. The panels keep
// their honest EmptyState until a trace-list channel exists.
//
// Latency/error aggregation over TraceSpan data resumes here once the
// backend exposes a recent-traces listing (see GetTraceDetail input need).

// Data loading
async function loadMonitorData() {
  try {
    const response = await getAgents()
    const agents = response.agents || []

    if (agents.length === 0) {
      dataAvailable.value = true
      useFallbackData()
      return
    }

    const metricsResults = await Promise.all(
      agents.map(a => getAgentMetrics(`${a.service_name}-${a.host}-${a.port}`)),
    )
    const metricsList = metricsResults.map(r => r.data)

    // Health overview
    const validMetrics = metricsList.filter((m): m is NonNullable<typeof m> => m !== null && m !== undefined)
    const avgSuccessRate = validMetrics.length > 0
      ? validMetrics.reduce((sum, m) => sum + (m.success_rate ?? 0), 0) / validMetrics.length
      : 0

    healthOverview.value = {
      score: Math.round(avgSuccessRate * 100),
      healthy: metricsList.filter(m => m && (m.success_rate ?? 0) >= 0.8).length,
      degraded: metricsList.filter(m => m && (m.success_rate ?? 0) >= 0.5 && (m.success_rate ?? 0) < 0.8).length,
      offline: metricsList.filter(m => !m || (m.success_rate ?? 0) < 0.5).length,
    }

    // Circuit breaker status (inferred from metrics)
    circuitBreakers.value = agents.map((agent, i) => {
      const m = metricsList[i]
      const rate = m?.success_rate ?? 0
      const totalReqs = m?.total_requests ?? 0
      return {
        agent: agent.service_name || 'Unknown',
        state: rate >= 0.8 ? 'CLOSED' : rate >= 0.5 ? 'HALF_OPEN' : 'OPEN',
        failures: m ? Math.round(totalReqs * (1 - rate)) : 0,
        lastTrigger: '--',
        recoveryTime: '--',
      }
    })

    dataAvailable.value = true
  } catch (e) {
    console.warn('Monitor API failed, using fallback', e)
    dataAvailable.value = false
    useFallbackData()
  }
}

// Refresh logic
const autoRefresh = ref(true)
const refreshInterval = ref(10)
const refreshing = ref(false)
let refreshTimer: ReturnType<typeof setInterval> | null = null

async function manualRefresh() {
  refreshing.value = true
  await loadMonitorData()
  refreshing.value = false
}

function startAutoRefresh() {
  stopAutoRefresh()
  refreshTimer = setInterval(() => { manualRefresh() }, refreshInterval.value * 1000)
}

function stopAutoRefresh() {
  if (refreshTimer) { clearInterval(refreshTimer); refreshTimer = null }
}

// Helpers
function stateLabel(state: string) {
  const map: Record<string, string> = { CLOSED: '正常', OPEN: '断开', HALF_OPEN: '半开' }
  return map[state] || state
}

// ECharts Options
const healthGaugeOption = computed(() => ({
  series: [{
    type: 'gauge',
    startAngle: 220,
    endAngle: -40,
    min: 0,
    max: 100,
    radius: '90%',
    progress: { show: true, width: 14, roundCap: true, itemStyle: { color: { type: 'linear', x: 0, y: 0, x2: 1, y2: 0, colorStops: [{ offset: 0, color: '#6366f1' }, { offset: 1, color: '#a855f7' }] } } },
    axisLine: { lineStyle: { width: 14, color: [[1, 'rgba(255,255,255,0.06)']] } },
    axisTick: { show: false },
    splitLine: { show: false },
    axisLabel: { show: false },
    pointer: { show: false },
    anchor: { show: false },
    title: { show: false },
    detail: {
      valueAnimation: true,
      fontSize: 28,
      fontWeight: 700,
      color: '#f1f5f9',
      offsetCenter: [0, 0],
      formatter: '{value}%',
    },
    data: [{ value: healthOverview.value.score }],
  }],
}))

// Watch refresh settings

// Latency distribution bar chart
const latencyDistributionOption = computed(() => ({
  backgroundColor: 'transparent',
  tooltip: {
    trigger: 'axis',
    axisPointer: { type: 'shadow' },
    backgroundColor: 'rgba(17, 24, 39, 0.95)',
    borderColor: 'rgba(255, 255, 255, 0.1)',
    textStyle: { color: '#f1f5f9', fontSize: 12 },
  },
  grid: { left: '3%', right: '4%', bottom: '3%', top: '8%', containLabel: true },
  xAxis: {
    type: 'category',
    data: latencyBuckets.value.map(b => b.range),
    axisLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.1)' } },
    axisLabel: { color: '#94a3b8', fontSize: 11 },
  },
  yAxis: {
    type: 'value',
    axisLine: { show: false },
    axisLabel: { color: '#94a3b8', fontSize: 11 },
    splitLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.05)' } },
  },
  series: [{
    type: 'bar',
    data: latencyBuckets.value.map(b => b.count),
    barWidth: '60%',
    itemStyle: {
      borderRadius: [4, 4, 0, 0],
      color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
        colorStops: [
          { offset: 0, color: '#6366f1' },
          { offset: 1, color: '#8b5cf6' }
        ]
      }
    }
  }]
}))

// Error rate trend chart
const errorRateOption = computed(() => ({
  backgroundColor: 'transparent',
  tooltip: {
    trigger: 'axis',
    backgroundColor: 'rgba(17, 24, 39, 0.95)',
    borderColor: 'rgba(255, 255, 255, 0.1)',
    textStyle: { color: '#f1f5f9', fontSize: 12 },
    formatter: (p: any) => `${p[0].name}: ${p[0].value}%`
  },
  grid: { left: '3%', right: '4%', bottom: '3%', top: '8%', containLabel: true },
  xAxis: {
    type: 'category',
    data: errorRateData.value.map(d => d.time),
    axisLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.1)' } },
    axisLabel: { color: '#94a3b8', fontSize: 11, rotate: 20 },
  },
  yAxis: {
    type: 'value',
    max: 100,
    axisLine: { show: false },
    axisLabel: { color: '#94a3b8', fontSize: 11, formatter: '{value}%' },
    splitLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.05)' } },
  },
  series: [{
    type: 'line',
    data: errorRateData.value.map(d => d.rate),
    smooth: true,
    symbol: 'circle',
    symbolSize: 6,
    lineStyle: { color: '#ef4444', width: 2 },
    itemStyle: { color: '#ef4444' },
    areaStyle: {
      color: { type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
        colorStops: [
          { offset: 0, color: 'rgba(239, 68, 68, 0.2)' },
          { offset: 1, color: 'rgba(239, 68, 68, 0.02)' }
        ]
      }
    }
  }]
}))

// Watch refresh settings
watch([autoRefresh, refreshInterval], ([isAuto]) => {
  if (isAuto) {
    startAutoRefresh()
  } else {
    stopAutoRefresh()
  }
})

// Lifecycle
onMounted(() => {
  loadMonitorData()
  if (autoRefresh.value) startAutoRefresh()
})

onUnmounted(() => {
  stopAutoRefresh()
})
</script>

<style scoped>
@import "../styles/design-tokens.css";

.monitor-page {
  min-height: 100vh;
  padding: var(--space-6);
  display: flex;
  flex-direction: column;
  gap: var(--space-5);
  background: var(--bg-primary);
}

/* Header */
.monitor-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: var(--space-4);
}

.page-title {
  font-size: 28px;
  font-weight: 800;
  letter-spacing: -0.02em;
}

.header-controls {
  display: flex;
  align-items: center;
  gap: var(--space-3);
}

.auto-refresh-toggle {
  display: flex;
  align-items: center;
  gap: var(--space-2);
}

.toggle-label {
  font-size: 13px;
  color: var(--text-secondary);
}

.toggle-btn {
  width: 40px;
  height: 22px;
  border-radius: var(--radius-full);
  border: none;
  background: var(--bg-elevated);
  cursor: pointer;
  position: relative;
  transition: background var(--duration-normal) var(--ease-default);
}

.toggle-btn.active {
  background: var(--brand-primary);
}

.toggle-knob {
  position: absolute;
  top: 3px;
  left: 3px;
  width: 16px;
  height: 16px;
  border-radius: 50%;
  background: #fff;
  transition: transform var(--duration-normal) var(--ease-default);
}

.toggle-btn.active .toggle-knob {
  transform: translateX(18px);
}

.interval-select {
  padding: 4px 8px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--border-default);
  background: var(--bg-elevated);
  color: var(--text-primary);
  font-size: 12px;
  cursor: pointer;
  outline: none;
}

.interval-select:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.refresh-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  height: 34px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--border-default);
  background: var(--bg-elevated);
  color: var(--text-secondary);
  cursor: pointer;
  transition: all var(--duration-normal) var(--ease-default);
}

.refresh-btn:hover {
  color: var(--text-primary);
  border-color: var(--border-strong);
}

.refresh-btn.spinning :deep(.iconify) {
  animation: spin-once 0.8s var(--ease-default);
}

@keyframes spin-once {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}

/* Connection Warning Banner */
.connection-warning {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-3) var(--space-4);
  border-radius: var(--radius-md);
  background: rgba(245, 158, 11, 0.1);
  border: 1px solid rgba(245, 158, 11, 0.3);
  color: var(--color-warning);
  font-size: 13px;
}

/* Rows */
.row {
  display: grid;
  gap: var(--space-5);
}

.row-3col {
  grid-template-columns: repeat(3, 1fr);
}

.row-2col {
  grid-template-columns: 2fr 1fr;
}

/* Panel */
.panel {
  min-height: 0;
}

.panel-title {
  font-size: 14px;
  font-weight: 600;
  color: var(--text-secondary);
  margin-bottom: var(--space-4);
  letter-spacing: 0.02em;
}

/* Chart containers */
.chart-container {
  height: 220px;
}

.gauge-wrap {
  height: 200px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.chart {
  width: 100%;
  height: 100%;
}

/* Health stats */
.health-stats {
  display: flex;
  justify-content: center;
  gap: var(--space-6);
  margin-top: var(--space-3);
}

.health-stat {
  display: flex;
  align-items: center;
  gap: var(--space-2);
}

.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.dot-success { background: var(--color-success); box-shadow: var(--shadow-glow-success); }
.dot-warning { background: var(--color-warning); box-shadow: var(--shadow-glow-warning); }
.dot-error { background: var(--color-error); box-shadow: var(--shadow-glow-error); }

.stat-num {
  font-size: 18px;
  font-weight: 700;
  color: var(--text-primary);
}

.stat-label {
  font-size: 12px;
  color: var(--text-tertiary);
}

/* Circuit Breaker */
.cb-grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: var(--space-3);
}

.cb-card {
  padding: var(--space-4);
  border-radius: var(--radius-md);
  background: var(--bg-surface);
  border: 1px solid var(--border-subtle);
  transition: all var(--duration-normal) var(--ease-default);
}

.cb-card:hover {
  background: var(--glass-bg-hover);
}

.cb-header {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  margin-bottom: var(--space-2);
}

.cb-indicator {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  flex-shrink: 0;
}

.indicator-closed {
  background: var(--color-success);
  animation: pulse-green 2s ease-in-out infinite;
}

.indicator-open {
  background: var(--color-error);
  animation: pulse-red 1.2s ease-in-out infinite;
}

.indicator-half_open {
  background: var(--color-warning);
  animation: pulse-yellow 1.5s ease-in-out infinite;
}

@keyframes pulse-green {
  0%, 100% { box-shadow: 0 0 4px rgba(16, 185, 129, 0.3); }
  50% { box-shadow: 0 0 14px rgba(16, 185, 129, 0.7); }
}

@keyframes pulse-red {
  0%, 100% { box-shadow: 0 0 4px rgba(239, 68, 68, 0.3); }
  50% { box-shadow: 0 0 14px rgba(239, 68, 68, 0.8); }
}

@keyframes pulse-yellow {
  0%, 100% { box-shadow: 0 0 4px rgba(245, 158, 11, 0.3); }
  50% { box-shadow: 0 0 14px rgba(245, 158, 11, 0.7); }
}

.cb-agent {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.cb-state-label {
  font-size: 11px;
  font-weight: 500;
  color: var(--text-tertiary);
  margin-bottom: var(--space-3);
}

.cb-details {
  display: flex;
  flex-direction: column;
  gap: var(--space-1);
}

.cb-detail {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
}

.cb-detail-label {
  color: var(--text-tertiary);
}

.cb-detail-value {
  color: var(--text-secondary);
  font-family: var(--font-mono);
}

/* Responsive */
@media (max-width: 1280px) {
  .row-3col {
    grid-template-columns: repeat(2, 1fr);
  }
  .row-3col > :last-child {
    grid-column: 1 / -1;
  }
}

@media (max-width: 1024px) {
  .row-3col {
    grid-template-columns: 1fr;
  }
  .row-3col > :last-child {
    grid-column: auto;
  }
  .row-2col {
    grid-template-columns: 1fr;
  }
  .cb-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

@media (max-width: 768px) {
  .monitor-page {
    padding: var(--space-4);
    gap: var(--space-4);
  }

  .monitor-header {
    flex-direction: column;
    align-items: flex-start;
  }

  .page-title {
    font-size: 22px;
  }

  .header-controls {
    width: 100%;
    flex-wrap: wrap;
  }

  .chart-container {
    height: 180px;
  }

  .gauge-wrap {
    height: 170px;
  }

  .cb-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 480px) {
  .monitor-page {
    padding: var(--space-3);
    gap: var(--space-3);
  }

  .page-title {
    font-size: 20px;
  }

  .chart-container {
    height: 160px;
  }

  .gauge-wrap {
    height: 150px;
  }

  .health-stats {
    gap: var(--space-4);
  }

  .stat-num {
    font-size: 16px;
  }

  .toggle-label {
    display: none;
  }
}

/* Reduced motion */
@media (prefers-reduced-motion: reduce) {
  .indicator-closed,
  .indicator-open,
  .indicator-half_open {
    animation: none;
  }

  .refresh-btn.spinning :deep(.iconify) {
    animation: none;
  }
}
</style>
