import { defineStore } from 'pinia'
import { ref } from 'vue'
import { getAgents, getAgentMetrics } from '../services/grpc-client'
import type { AgentDisplayInfo, AgentMetrics, ServiceInfo } from '../types/proto'

export const useAgentsStore = defineStore('agents', () => {
  const agents = ref<AgentDisplayInfo[]>([])
  const loading = ref(false)
  const lastFetched = ref<number>(0)
  const error = ref<string>('')

  async function fetchAgents() {
    loading.value = true
    error.value = ''

    try {
      const resp = await getAgents()
      if (resp.status.code !== 0) {
        throw new Error(resp.status.message || 'Failed to fetch agents')
      }
      agents.value = resp.agents.map(toAgentDisplayInfo)
      lastFetched.value = Date.now()
    } catch (err: unknown) {
      error.value = err instanceof Error ? err.message : 'Failed to fetch agents'
    } finally {
      loading.value = false
    }
  }

  /**
   * Fetch runtime metrics for the given agent
   * Returns a wrapper: { data: AgentMetrics | null, error?: string }
   * data is null with an error when the call fails or the backend returns empty
   */
  async function fetchAgentMetrics(
    agentId: string,
  ): Promise<{ data: AgentMetrics | null; error?: string }> {
    try {
      const result = await getAgentMetrics(agentId)
      if (result.error) {
        error.value = result.error
      }
      return result
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to fetch agent metrics'
      error.value = msg
      return { data: null, error: msg }
    }
  }

  function toAgentDisplayInfo(info: ServiceInfo): AgentDisplayInfo {
    let cardSkills: string[] = []
    try {
      if (info.agent_card) {
        const card = JSON.parse(info.agent_card)
        if (card.skills) {
          cardSkills = card.skills.map(
            (s: { name: string }) => s.name,
          )
        }
      }
    } catch {
      // agent_card JSON parse failed, fallback to skills field
    }

    const status = info.health_status || ''
    return {
      id: info.service_name,
      name: info.service_name,
      host: info.host,
      port: info.port,
      version: info.version,
      tags: info.tags || [],
      skills: cardSkills.length > 0 ? cardSkills : info.skills || [],
      // Registered via GetAgents = currently reachable; the health verdict
      // decides routability: UNHEALTHY/offline are excluded from routing,
      // DEGRADED stays routable (observability-only signal).
      healthStatus: status || 'UNKNOWN',
      healthy: status !== 'UNHEALTHY' && status !== 'offline',
    }
  }

  // Auto-polling: refresh every 15 seconds
  let pollTimer: ReturnType<typeof setInterval> | null = null

  function startPolling(intervalMs = 15000) {
    stopPolling()
    fetchAgents()
    pollTimer = setInterval(fetchAgents, intervalMs)
  }

  function stopPolling() {
    if (pollTimer) {
      clearInterval(pollTimer)
      pollTimer = null
    }
  }

  return {
    agents,
    loading,
    lastFetched,
    error,
    fetchAgents,
    fetchAgentMetrics,
    startPolling,
    stopPolling,
  }
})
