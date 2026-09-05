<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from 'vue'
import { Icon } from '@iconify/vue'
import { CountUp } from 'countup.js'
import GlassCard from '../components/layout/GlassCard.vue'
import PageLoader from '../components/feedback/PageLoader.vue'
import EmptyState from '../components/feedback/EmptyState.vue'
import VChart from 'vue-echarts'
import { use } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { LineChart, BarChart, PieChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, LegendComponent, TitleComponent } from 'echarts/components'
import { getAgents, getAgentMetrics, getCostReport } from '../services/grpc-client'
import { useAuthStore } from '../stores/auth'
import type { AgentMetrics, ServiceInfo } from '../types/proto'

use([CanvasRenderer, LineChart, BarChart, PieChart, GridComponent, TooltipComponent, LegendComponent, TitleComponent])

// State
const loading = ref(true)
const dataAvailable = ref(false)
const observabilityAvailable = ref(false)
let refreshTimer: ReturnType<typeof setInterval> | null = null
const authStore = useAuthStore()

// Stats cards
const stats = ref([
  { label: '总请求数', value: null as number | null, icon: 'mdi:chart-bar', trend: null as number | null, trendUp: true, display: '--', unsupported: false },
  { label: '活跃Agent', value: null as number | null, icon: 'mdi:robot-happy', trend: null as number | null, trendUp: true, display: '--', unsupported: false },
  { label: 'Token消耗', value: null as number | null, icon: 'mdi:lightning-bolt', trend: null as number | null, trendUp: true, display: '--', unsupported: false },
  { label: '今日成本', value: null as number | null, icon: 'mdi:currency-usd', trend: null as number | null, trendUp: false, display: '--', unsupported: false },
])

// Agent ranking data
const agentRanking = ref<{ name: string; count: number }[]>([])

// Cost distribution (populated from agent metrics + cost report when available)
const costDistribution = ref<{ name: string; value: number }[]>([])

// Token trend (populated from ObservabilityService GetCostReport)
const tokenTrendAvailable = ref(false)
const tokenTrend = ref({
  dates: [] as string[],
  input: [] as number[],
  output: [] as number[],
})

// Activities (no backend API yet — empty state shown)
const recentActivities = ref<any[]>([])

// CountUp refs
const counterRefs = ref<(HTMLElement | null)[]>([null, null, null, null])
const countUpInstances = ref<(CountUp | null)[]>([null, null, null, null])

// Fallback data (zero/empty state when API unavailable)
function useFallbackData() {
  // Reset to empty/zero state
  stats.value[0].value = null
  stats.value[0].trend = null
  stats.value[0].display = '--'
  stats.value[1].value = null
  stats.value[1].trend = null
  stats.value[1].display = '--'
  stats.value[2].value = null
  stats.value[2].display = '--'
  stats.value[3].value = null
  stats.value[3].display = '--'

  agentRanking.value = []
}

// Load Observability data (cost + token trend)
async function loadObservabilityData() {
  const userId = authStore.userId
  if (!userId) return

  const today = new Date().toISOString().slice(0, 10)

  // Today's cost report for stats cards
  const todayReport = await getCostReport(userId, today, today)
  if (todayReport && todayReport.records.length > 0) {
    const rec = todayReport.records[0]
    const totalTokens = rec.total_prompt_tokens + rec.total_completion_tokens
    stats.value[2].value = totalTokens
    stats.value[2].display = totalTokens.toLocaleString()
    stats.value[3].value = rec.total_cost_usd
    stats.value[3].display = `$${rec.total_cost_usd.toFixed(2)}`
    observabilityAvailable.value = true
  } else if (todayReport) {
    // API responded but no records for today
    stats.value[2].value = 0
    stats.value[2].display = '0'
    stats.value[3].value = 0
    stats.value[3].display = '$0.00'
    observabilityAvailable.value = true
  }

  // 30-day token trend
  const thirtyDaysAgo = new Date(Date.now() - 30 * 86400000).toISOString().slice(0, 10)
  const trendReport = await getCostReport(userId, thirtyDaysAgo, today)
  if (trendReport && trendReport.records.length > 0) {
    // Sort records by date ascending
    const sorted = [...trendReport.records].sort((a, b) => a.date.localeCompare(b.date))
    tokenTrend.value.dates = sorted.map(r => r.date.slice(5)) // MM-DD
    tokenTrend.value.input = sorted.map(r => r.total_prompt_tokens)
    tokenTrend.value.output = sorted.map(r => r.total_completion_tokens)
    tokenTrendAvailable.value = true
  }
}

// Load real data
async function loadDashboardData() {
  loading.value = true
  try {
    // Get agent list
    const response = await getAgents()
    const agents: ServiceInfo[] = response.agents || []

    // Concurrently fetch metrics for all agents
    // getAgentMetrics returns { data: AgentMetrics | null; error?: string }
    // Metrics are keyed by the server registry id "service_name-host-port".
    const metricsResults = await Promise.all(
      agents.map(a => getAgentMetrics(`${a.service_name}-${a.host}-${a.port}`)),
    )

    // Extract the AgentMetrics from each result wrapper
    const metricsList: (AgentMetrics | null)[] = metricsResults.map(r => r.data)

    // Compute stats
    const totalRequests = metricsList.reduce((sum, m) => sum + (m?.total_requests || 0), 0)
    const activeAgents = agents.length

    stats.value[0].value = totalRequests
    stats.value[0].trend = null // no historical comparison yet
    stats.value[0].display = totalRequests.toLocaleString()

    stats.value[1].value = activeAgents
    stats.value[1].trend = null
    stats.value[1].display = String(activeAgents)

    // Observability: cost & token data (non-blocking, graceful degradation)
    try {
      await loadObservabilityData()
    } catch (e) {
      console.warn('Observability data failed, keeping defaults', e)
    }

    // Cost distribution: use agent request proportions as proxy
    const totalReqs = metricsList.reduce((sum, m) => sum + (m?.total_requests || 0), 0)
    if (totalReqs > 0 && observabilityAvailable.value && stats.value[3].value != null) {
      const totalCost = stats.value[3].value as number
      costDistribution.value = agents
        .map((a, i) => ({
          name: a.service_name || `Agent-${i}`,
          value: totalReqs > 0
            ? Math.round(((metricsList[i]?.total_requests || 0) / totalReqs) * totalCost * 100) / 100
            : 0,
        }))
        .filter(c => c.value > 0)
    }

    // Agent ranking - real data sorted by total_requests desc
    agentRanking.value = agents
      .map((a, i) => ({
        name: a.service_name || `Agent-${i}`,
        count: metricsList[i]?.total_requests || 0,
      }))
      .sort((a, b) => b.count - a.count)

    dataAvailable.value = true
  } catch (e) {
    console.warn('Dashboard API failed, using fallback', e)
    dataAvailable.value = false
    useFallbackData()
  } finally {
    loading.value = false
  }
}

// Start CountUp animations after data loads
function startCountUp() {
  // Destroy previous instances
  countUpInstances.value.forEach(cu => cu?.reset?.())
  countUpInstances.value = [null, null, null, null]

  nextTick(() => {
    stats.value.forEach((stat, i) => {
      const el = counterRefs.value[i]
      if (!el || stat.value == null) return

      const target = stat.value
      let options: Record<string, any> = { duration: 2, useEasing: true }

      if (i === 0) {
        options = { ...options, separator: ',' }
      } else if (i === 3) {
        options = { ...options, decimalPlaces: 2, prefix: '$', separator: ',' }
      }

      const cu = new CountUp(el, target, options as any)
      countUpInstances.value[i] = cu
      if (!cu.error) cu.start()
    })
  })
}

watch(loading, (val) => {
  if (!val) startCountUp()
})

onMounted(() => {
  loadDashboardData()
  refreshTimer = setInterval(loadDashboardData, 30000)
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})

// Chart options

// Token trend chart (empty state when no data)
const tokenTrendOption = computed(() => ({
  backgroundColor: 'transparent',
  tooltip: {
    trigger: 'axis',
    backgroundColor: 'rgba(17, 24, 39, 0.95)',
    borderColor: 'rgba(255, 255, 255, 0.1)',
    textStyle: { color: '#f1f5f9', fontSize: 12 },
  },
  legend: {
    data: ['Input Tokens', 'Output Tokens'],
    top: 10,
    textStyle: { color: '#94a3b8', fontSize: 12 }
  },
  grid: { left: '3%', right: '4%', bottom: '3%', top: '18%', containLabel: true },
  xAxis: {
    type: 'category',
    data: tokenTrend.value.dates,
    axisLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.1)' } },
    axisLabel: { color: '#94a3b8', fontSize: 11 },
    boundaryGap: false
  },
  yAxis: {
    type: 'value',
    axisLine: { show: false },
    axisLabel: {
      color: '#94a3b8', fontSize: 11,
      formatter: (v: number) => (v / 1000).toFixed(0) + 'K'
    },
    splitLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.05)' } }
  },
  series: [
    {
      name: 'Input Tokens', type: 'line', data: tokenTrend.value.input,
      smooth: true, symbol: 'circle', symbolSize: 6,
      lineStyle: { color: '#6366f1', width: 3 }, itemStyle: { color: '#6366f1' },
      areaStyle: {
        color: {
          type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
          colorStops: [
            { offset: 0, color: 'rgba(99, 102, 241, 0.25)' },
            { offset: 1, color: 'rgba(99, 102, 241, 0.02)' }
          ]
        }
      }
    },
    {
      name: 'Output Tokens', type: 'line', data: tokenTrend.value.output,
      smooth: true, symbol: 'circle', symbolSize: 6,
      lineStyle: { color: '#8b5cf6', width: 3 }, itemStyle: { color: '#8b5cf6' },
      areaStyle: {
        color: {
          type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
          colorStops: [
            { offset: 0, color: 'rgba(139, 92, 246, 0.25)' },
            { offset: 1, color: 'rgba(139, 92, 246, 0.02)' }
          ]
        }
      }
    }
  ]
}))

// Agent ranking chart
const agentRankingOption = computed(() => ({
  backgroundColor: 'transparent',
  tooltip: {
    trigger: 'axis',
    axisPointer: { type: 'shadow' },
    backgroundColor: 'rgba(17, 24, 39, 0.95)',
    borderColor: 'rgba(255, 255, 255, 0.1)',
    textStyle: { color: '#f1f5f9', fontSize: 12 }
  },
  grid: { left: '3%', right: '10%', bottom: '3%', top: '3%', containLabel: true },
  xAxis: {
    type: 'value',
    axisLine: { show: false },
    axisLabel: { color: '#94a3b8', fontSize: 11 },
    splitLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.05)' } }
  },
  yAxis: {
    type: 'category',
    data: agentRanking.value.map(a => a.name).reverse(),
    axisLine: { lineStyle: { color: 'rgba(255, 255, 255, 0.1)' } },
    axisLabel: { color: '#94a3b8', fontSize: 12 }
  },
  series: [
    {
      type: 'bar',
      data: agentRanking.value.map(a => a.count).reverse(),
      barWidth: '60%',
      itemStyle: {
        borderRadius: [0, 6, 6, 0],
        color: {
          type: 'linear', x: 0, y: 0, x2: 1, y2: 0,
          colorStops: [
            { offset: 0, color: '#6366f1' },
            { offset: 1, color: '#a855f7' }
          ]
        }
      }
    }
  ]
}))

// Cost distribution pie chart
const totalCost = computed(() => costDistribution.value.reduce((s, c) => s + c.value, 0))

const costPieOption = computed(() => ({
  backgroundColor: 'transparent',
  tooltip: {
    trigger: 'item',
    backgroundColor: 'rgba(17, 24, 39, 0.95)',
    borderColor: 'rgba(255, 255, 255, 0.1)',
    textStyle: { color: '#f1f5f9', fontSize: 12 },
    formatter: (p: any) => `${p.name}: $${p.value.toFixed(2)} (${p.percent}%)`
  },
  legend: {
    orient: 'vertical', right: '5%', top: 'center',
    textStyle: { color: '#94a3b8', fontSize: 12 }
  },
  series: [
    {
      type: 'pie',
      radius: ['50%', '75%'],
      center: ['35%', '50%'],
      avoidLabelOverlap: false,
      label: { show: false },
      emphasis: { label: { show: false } },
      data: costDistribution.value.map((c, i) => ({
        ...c,
        itemStyle: { color: ['#6366f1', '#8b5cf6', '#10b981', '#f59e0b', '#3b82f6'][i] }
      }))
    }
  ],
  graphic: [
    {
      type: 'text', left: '28%', top: '44%',
      style: {
        text: `$${totalCost.value.toFixed(2)}`,
        textAlign: 'center', fill: '#f1f5f9', fontSize: 20, fontWeight: 'bold'
      }
    },
    {
      type: 'text', left: '28%', top: '54%',
      style: { text: '总成本', textAlign: 'center', fill: '#94a3b8', fontSize: 12 }
    }
  ]
}))

// Sparkline options for stat cards
const getSparklineOption = (data: number[], color: string) => ({
  backgroundColor: 'transparent',
  grid: { left: 0, right: 0, top: 0, bottom: 0 },
  xAxis: { type: 'category', show: false, data: data.map((_, i) => i) },
  yAxis: { type: 'value', show: false },
  series: [{
    type: 'line', data, smooth: true, symbol: 'none',
    lineStyle: { color, width: 2 },
    areaStyle: {
      color: {
        type: 'linear', x: 0, y: 0, x2: 0, y2: 1,
        colorStops: [
          { offset: 0, color: color.replace(')', ', 0.3)').replace('rgb', 'rgba') },
          { offset: 1, color: color.replace(')', ', 0.05)').replace('rgb', 'rgba') }
        ]
      }
    }
  }]
})

const sparklineData = [
  [] as number[],
  [] as number[],
  [] as number[],
  [] as number[],
]
</script>

<template>
  <PageLoader v-if="loading" :count="6" />
  <div v-else class="dashboard-page">
    <!-- API connection warning -->
    <div v-if="!dataAvailable" class="api-warning">
      <Icon icon="mdi:alert-circle-outline" width="18" />
      <span>后端服务未连接，数据暂不可用</span>
    </div>
    <div v-else-if="!observabilityAvailable" class="api-warning">
      <Icon icon="mdi:alert-circle-outline" width="18" />
      <span>ObservabilityService 不可用，Token消耗和成本数据暂不可用</span>
    </div>

    <div class="page-header">
      <h1 class="page-title">数据面板</h1>
      <p class="page-subtitle">实时监控 Agent 系统运行状态</p>
    </div>

    <!-- Top: 4 stat cards -->
    <div class="stats-grid">
      <GlassCard
        v-for="(stat, index) in stats"
        :key="stat.label"
        hoverable
        glow
        class="stat-card"
        :style="{ animationDelay: `${index * 100}ms` }"
      >
        <div class="stat-content">
          <div class="stat-left">
            <div class="stat-icon">
              <Icon :icon="stat.icon" width="24" />
            </div>
            <div v-if="stat.unsupported && stat.value == null" class="stat-value stat-value-na">
              --
              <div class="stat-hint">{{ (stat as any).hint || '需后端支持' }}</div>
            </div>
            <div v-else class="stat-value" :ref="(el: any) => counterRefs[index] = el">
              {{ stat.value == null ? '--' : '0' }}
            </div>
            <div class="stat-label">{{ stat.label }}</div>
          </div>
          <div class="stat-right">
            <div v-if="stat.trend != null" class="stat-trend" :class="stat.trendUp ? 'trend-up' : 'trend-down'">
              <Icon :icon="stat.trendUp ? 'mdi:arrow-up' : 'mdi:arrow-down'" width="14" />
              <span>{{ Math.abs(stat.trend) }}%</span>
            </div>
            <VChart
              v-if="!stat.unsupported"
              :option="getSparklineOption(sparklineData[index], stat.trendUp ? '#10b981' : '#ef4444')"
              class="sparkline"
              style="height: 40px; width: 80px;"
            />
          </div>
        </div>
      </GlassCard>
    </div>

    <!-- Middle: two columns -->
    <div class="middle-grid">
      <GlassCard class="chart-card token-trend-card" :style="{ animation: 'chartFadeScale 0.5s var(--ease-out) 200ms both' }">
        <div class="chart-header">
          <h3 class="chart-title">Token消耗趋势</h3>
          <span class="chart-subtitle">最近7天</span>
        </div>
        <VChart v-if="tokenTrendAvailable" :option="tokenTrendOption" autoresize style="height: 300px;" />
        <EmptyState
          v-else
          icon="mdi:chart-timeline-variant"
          title="Token趋势数据暂不可用"
          description="Token消耗趋势需要后端ObservabilityService支持"
        />
      </GlassCard>

      <GlassCard class="chart-card ranking-card" :style="{ animation: 'chartFadeScale 0.5s var(--ease-out) 300ms both' }">
        <div class="chart-header">
          <h3 class="chart-title">Agent调用排行</h3>
          <span class="chart-subtitle">按调用次数</span>
        </div>
        <VChart v-if="agentRanking.length > 0" :option="agentRankingOption" autoresize style="height: 300px;" />
        <EmptyState
          v-else
          icon="mdi:trophy-outline"
          title="暂无调用数据"
          description="当有Agent处理请求后将显示调用排行"
        />
      </GlassCard>
    </div>

    <!-- Bottom: two columns -->
    <div class="bottom-grid">
      <GlassCard class="chart-card pie-card" :style="{ animation: 'chartFadeScale 0.5s var(--ease-out) 400ms both' }">
        <div class="chart-header">
          <h3 class="chart-title">成本分布</h3>
          <span class="chart-subtitle">按Agent分类</span>
        </div>
        <VChart v-if="costDistribution.length > 0" :option="costPieOption" autoresize style="height: 300px;" />
        <EmptyState
          v-else
          icon="mdi:chart-donut"
          title="成本数据暂不可用"
          description="成本分布需要后端ObservabilityService支持"
        />
      </GlassCard>

      <GlassCard class="chart-card activity-card" :style="{ animation: 'chartFadeScale 0.5s var(--ease-out) 500ms both' }">
        <div class="chart-header">
          <h3 class="chart-title">最近活动</h3>
          <span class="chart-subtitle">实时动态</span>
        </div>
        <div v-if="recentActivities.length > 0" class="activity-timeline">
          <div
            v-for="(activity, index) in recentActivities"
            :key="index"
            class="activity-item"
            :style="{ animationDelay: `${index * 80}ms` }"
          >
            <div class="activity-dot" :class="`status-${activity.status}`" />
            <div class="activity-content">
              <div class="activity-main">
                <span class="activity-agent">{{ activity.agent }}</span>
                <span class="activity-action">{{ activity.action }}</span>
              </div>
              <div class="activity-meta">
                <span class="activity-time">{{ activity.time }}</span>
                <span class="activity-duration">{{ activity.duration }}</span>
              </div>
            </div>
          </div>
        </div>
        <EmptyState
          v-else
          icon="mdi:timeline-clock-outline"
          title="暂无活动记录"
          description="活动记录需要后端支持"
        />
      </GlassCard>
    </div>
  </div>
</template>

<style scoped>
@import "../styles/design-tokens.css";

.dashboard-page {
  max-width: 1400px;
  margin: 0 auto;
  display: flex;
  flex-direction: column;
  gap: var(--space-6);
}

/* API warning banner */
.api-warning {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-3) var(--space-4);
  border-radius: var(--radius-md);
  background: rgba(245, 158, 11, 0.12);
  border: 1px solid rgba(245, 158, 11, 0.25);
  color: #f59e0b;
  font-size: 13px;
}

.page-header {
  margin-bottom: var(--space-2);
}

.page-title {
  font-size: 28px;
  font-weight: 700;
  color: var(--text-primary);
  margin-bottom: var(--space-1);
}

.page-subtitle {
  font-size: 14px;
  color: var(--text-secondary);
}

/* Stats Grid */
.stats-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: var(--space-4);
}

.stat-card {
  cursor: default;
  animation: statSlideUp 0.5s var(--ease-out) both;
}

@keyframes statSlideUp {
  from {
    opacity: 0;
    transform: translateY(20px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

.stat-content {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
}

.stat-left {
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
}

.stat-icon {
  width: 40px;
  height: 40px;
  border-radius: var(--radius-md);
  background: var(--brand-gradient);
  display: flex;
  align-items: center;
  justify-content: center;
  color: white;
  position: relative;
  overflow: hidden;
}

.stat-icon::after {
  content: '';
  position: absolute;
  inset: 0;
  background: linear-gradient(135deg, rgba(255,255,255,0.15), transparent 50%);
  pointer-events: none;
}

.stat-value {
  font-size: 28px;
  font-weight: 700;
  color: var(--text-primary);
  font-family: var(--font-mono);
  line-height: 1;
}

.stat-value-na {
  color: var(--text-tertiary);
}

.stat-hint {
  font-size: 11px;
  font-weight: 400;
  color: var(--text-tertiary);
  font-family: inherit;
  margin-top: 4px;
}

.stat-label {
  font-size: 13px;
  color: var(--text-secondary);
}

.stat-right {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: var(--space-2);
}

.stat-trend {
  display: flex;
  align-items: center;
  gap: 2px;
  font-size: 12px;
  font-weight: 600;
  padding: 2px 8px;
  border-radius: var(--radius-full);
}

.trend-up {
  color: var(--color-success);
  background: rgba(16, 185, 129, 0.1);
}

.trend-down {
  color: var(--color-error);
  background: rgba(239, 68, 68, 0.1);
}

.sparkline {
  opacity: 0.8;
}

/* Middle Grid */
.middle-grid {
  display: grid;
  grid-template-columns: 2fr 1fr;
  gap: var(--space-4);
}

/* Bottom Grid */
.bottom-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: var(--space-4);
}

/* Chart Cards */
.chart-card {
  display: flex;
  flex-direction: column;
}

@keyframes chartFadeScale {
  from { opacity: 0; transform: scale(0.96); }
  to { opacity: 1; transform: scale(1); }
}

.chart-header {
  display: flex;
  align-items: baseline;
  gap: var(--space-3);
  margin-bottom: var(--space-4);
  padding-bottom: var(--space-3);
  border-bottom: 1px solid var(--border-subtle);
  position: relative;
}

.chart-header::after {
  content: '';
  position: absolute;
  left: 0;
  bottom: -1px;
  width: 40px;
  height: 2px;
  background: var(--brand-gradient);
  border-radius: 1px;
}

.chart-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--text-primary);
}

.chart-subtitle {
  font-size: 12px;
  color: var(--text-tertiary);
}

/* Activity Timeline */
.activity-timeline {
  display: flex;
  flex-direction: column;
  gap: var(--space-3);
  max-height: 300px;
  overflow-y: auto;
}

.activity-item {
  display: flex;
  align-items: flex-start;
  gap: var(--space-3);
  padding: var(--space-3);
  border-radius: var(--radius-md);
  background: var(--bg-surface);
  border: 1px solid var(--border-subtle);
  animation: slideInLeft 0.4s var(--ease-out) both;
  transition: all var(--duration-fast) var(--ease-default);
}

.activity-item:hover {
  background: var(--glass-bg-hover);
  border-color: var(--border-strong);
  transform: translateX(4px);
}

@keyframes slideInLeft {
  from {
    opacity: 0;
    transform: translateX(-20px);
  }
  to {
    opacity: 1;
    transform: translateX(0);
  }
}

.activity-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  margin-top: 6px;
  flex-shrink: 0;
}

.status-success {
  background: var(--color-success);
  box-shadow: 0 0 8px rgba(16, 185, 129, 0.4);
}

.status-warning {
  background: var(--color-warning);
  box-shadow: 0 0 8px rgba(245, 158, 11, 0.4);
}

.status-error {
  background: var(--color-error);
  box-shadow: 0 0 8px rgba(239, 68, 68, 0.4);
}

.activity-content {
  flex: 1;
  min-width: 0;
}

.activity-main {
  font-size: 13px;
  color: var(--text-primary);
  margin-bottom: 4px;
}

.activity-agent {
  font-weight: 600;
  margin-right: var(--space-2);
}

.activity-action {
  color: var(--text-secondary);
}

.activity-meta {
  display: flex;
  gap: var(--space-3);
  font-size: 12px;
  color: var(--text-tertiary);
}

.activity-duration {
  font-family: var(--font-mono);
}

/* Responsive */
@media (max-width: 1024px) {
  .stats-grid {
    grid-template-columns: repeat(2, 1fr);
  }
  .middle-grid,
  .bottom-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 640px) {
  .stats-grid {
    grid-template-columns: 1fr;
  }
  .dashboard-page {
    padding: var(--space-4);
  }
}

@media (max-width: 480px) {
  .dashboard-page {
    padding: var(--space-3);
    gap: var(--space-3);
  }
}

/* Reduced motion */
@media (prefers-reduced-motion: reduce) {
  .stat-card {
    animation: none;
  }
  .activity-item {
    animation: none;
  }
  .chart-card {
    animation: none !important;
  }
}
</style>
