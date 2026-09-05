/**
 * JSON proxy client wrapper
 *
 * Communicates with the Node JSON→gRPC proxy (gateway/proxy/server.mjs)
 * over HTTP/1.1. Currently uses hand-written TypeScript types + fetch;
 * can switch to protoc-generated stubs later.
 */

import type {
  AIQueryRequest,
  AIQueryResponse,
  AIStreamEvent,
  AgentMetrics,
  GetAgentMetricsResponse,
  GetAgentsResponse,
  FindAgentsRequest,
  FindAgentsResponse,
  RegisterRequest,
  RegisterResponse,
  LoginRequest,
  LoginResponse,
  GetTraceDetailResponse,
  GetCostReportResponse,
  ReplayQueryRequest,
  ReplayQueryResponse,
  ExportConversationRequest,
  ExportConversationResponse,
  DAGStructure,
  ExecutePlanRequest,
  ExecutePlanResponse,
  ShareSessionRequest,
  ShareSessionResponse,
  ReadSharedConversationRequest,
  ReadSharedConversationResponse,
  ListSharesResponse,
  RevokeShareResponse,
  SaveTemplateRequest,
  SaveTemplateResponse,
  ListTemplatesResponse,
  GetTemplateResponse,
  UseTemplateResponse,
  SandboxQueryRequest,
  SandboxQueryResponse,
  InterventionResponseRequest,
  InterventionResponseResponse,
  CompareAgentsRequest,
  CompareAgentsResponse,
  GetAgentCompareRequest,
  GetAgentCompareResponse,
  SetAutonomyLevelRequest,
  SetAutonomyLevelResponse,
  UndoActionRequest,
  UndoActionResponse,
} from '../types/proto'

const BASE_URL = import.meta.env.VITE_API_BASE || ''

// AIQueryService path prefix
const AI_QUERY = '/agent_communication.AIQueryService'
const AGENT_COMM = '/agent_communication.AgentCommunicationService'
const USER_AUTH = '/agent_communication.auth.UserService'
const OBSERVABILITY = '/agent_communication.ObservabilityService'
const ORCHESTRATION = '/agent_communication.OrchestrationService'
const SHARING = '/agent_communication.SharingService'
const USER_EXPERIENCE = '/agent_communication.UserExperienceService'
const AGENT_LIFECYCLE = '/agent_communication.AgentLifecycleService'

// Auth token accessor (set by auth store)
let _getAuthToken: (() => string | null) | null = null
export function setAuthTokenGetter(getter: () => string | null) {
  _getAuthToken = getter
}

// Unauthorized callback (set by auth store to trigger logout on 401)
let _onUnauthorized: (() => void) | null = null
export function setOnUnauthorized(cb: () => void) { _onUnauthorized = cb }

/**
 * Unary RPC call (JSON serialized, forwarded by the Node JSON proxy)
 */
async function unaryCall<TReq, TRes>(
  servicePath: string,
  method: string,
  request: TReq,
): Promise<TRes> {
  const url = `${BASE_URL}${servicePath}/${method}`

  const headers: Record<string, string> = { 'Content-Type': 'application/json' }
  const token = _getAuthToken?.()
  if (token) {
    headers['Authorization'] = `Bearer ${token}`
  }

  const resp = await fetch(url, {
    method: 'POST',
    headers,
    body: JSON.stringify(request),
  })

  if (!resp.ok) {
    if (resp.status === 401) {
      _onUnauthorized?.()
    }
    // The proxy returns the real gRPC error as { error, code, details }.
    // Surface that semantic instead of a bare HTTP status.
    let backendMessage = ''
    try {
      const body = await resp.json()
      backendMessage = (body && (body.error || body.message || body.details)) || ''
    } catch {
      // non-JSON error body
    }
    throw new Error(
      backendMessage || `RPC ${method} failed: ${resp.status} ${resp.statusText}`,
    )
  }

  return resp.json() as Promise<TRes>
}

// AIQueryService

/** Synchronous query */
export async function query(
  question: string,
  contextId?: string,
): Promise<AIQueryResponse> {
  const req: AIQueryRequest = {
    request_id: crypto.randomUUID(),
    question,
    context_id: contextId || 'default',
    history_length: 5,
    timeout_seconds: 60,
    metadata: {},
  }
  return unaryCall<AIQueryRequest, AIQueryResponse>(AI_QUERY, 'Query', req)
}

/**
 * Streaming query
 *
 * Uses fetch + ReadableStream to consume server-streaming responses.
 * The JSON proxy returns server-streaming RPCs as SSE frames,
 * simplified here to line-by-line JSON event parsing.
 */
export function queryStream(
  question: string,
  onEvent: (event: AIStreamEvent) => void,
  contextId?: string,
  signal?: AbortSignal,
  planOnly?: boolean,
): Promise<void> {
  const req: AIQueryRequest = {
    request_id: crypto.randomUUID(),
    question,
    context_id: contextId || 'default',
    history_length: 5,
    timeout_seconds: 120,
    metadata: {},
    // B1/U4 two-phase: stop after planning; execution is driven by a
    // follow-up ExecutePlan call after user confirmation.
    ...(planOnly ? { plan_only: true } : {}),
  }

  const url = `${BASE_URL}${AI_QUERY}/QueryStream`

  const headers: Record<string, string> = { 'Content-Type': 'application/json' }
  const token = _getAuthToken?.()
  if (token) {
    headers['Authorization'] = `Bearer ${token}`
  }

  const controller = new AbortController()
  let timedOut = false
  const timeoutId = setTimeout(() => { timedOut = true; controller.abort() }, 130_000)
  // External-signal listener must be removable: the fetch/read loop outlives
  // the abort decision, and a leaked closure pins the controller/reader for
  // every Start/Stop cycle of a long-lived session.
  const onExternalAbort = () => controller.abort()
  if (signal) {
    signal.addEventListener('abort', onExternalAbort, { once: true })
  }
  const detachExternalAbort = () => {
    if (signal) signal.removeEventListener('abort', onExternalAbort)
  }

  return fetch(url, {
    method: 'POST',
    headers,
    body: JSON.stringify(req),
    signal: controller.signal,
  })
    .then(async (resp) => {
      if (!resp.ok) {
        // Same contract as unaryCall: trigger logout on 401 and surface the
        // proxy's semantic { error, code, details } body.
        if (resp.status === 401) {
          _onUnauthorized?.()
        }
        let backendMessage = ''
        try {
          const body = await resp.json()
          backendMessage = (body && (body.error || body.message || body.details)) || ''
        } catch {
          // non-JSON error body
        }
        throw new Error(backendMessage || `QueryStream failed: ${resp.status}`)
      }

      const reader = resp.body?.getReader()
      if (!reader) throw new Error('No response body')

      const decoder = new TextDecoder()
      let buffer = ''

      while (true) {
        const { done, value } = await reader.read()
        if (done) break

        buffer += decoder.decode(value, { stream: true })
        // Split by newline, process SSE-style frames
        const lines = buffer.split('\n')
        buffer = lines.pop() || '' // last line may be incomplete

        for (const line of lines) {
          const trimmed = line.trim()
          if (!trimmed || trimmed.startsWith(':')) continue
          // Try parsing data: prefix (SSE format) or direct JSON
          const jsonStr = trimmed.startsWith('data: ')
            ? trimmed.slice(6)
            : trimmed
          try {
            const event = JSON.parse(jsonStr) as AIStreamEvent & { code_name?: string }
            // SSE delivery has no HTTP status channel: the proxy turns an
            // UNAUTHENTICATED stream failure into an in-band error event.
            // Recognize it here so an expired token triggers the same logout
            // flow as the unary resp.status===401 branch instead of leaving
            // the user in a "fake logged-in" state.
            if (event.event_type === 'error' && event.code_name === 'UNAUTHENTICATED') {
              _onUnauthorized?.()
            }
            onEvent(event)
          } catch {
            // Non-JSON line: surface it (a producer format change used to
            // degrade into "no events until connection lost" silently).
            console.warn('[grpc-client] unparseable stream line skipped:', trimmed.slice(0, 80))
          }
        }
      }

      // Process remaining content in buffer
      if (buffer.trim()) {
        try {
          const event = JSON.parse(buffer.trim()) as AIStreamEvent
          onEvent(event)
        } catch {
          // ignore
        }
      }
      clearTimeout(timeoutId)
      detachExternalAbort()
    })
    .catch((err) => {
      clearTimeout(timeoutId)
      detachExternalAbort()
      // Emit error for timeout abort or non-abort errors; skip only user-initiated abort
      if (err.name === 'AbortError' && !timedOut) return
      onEvent({
        event_id: '',
        event_type: 'error',
        content: timedOut ? 'Request timed out, please try again' : (err.message || 'Stream failed'),
        task_state: 'failed',
        context_id: contextId || 'default',
        timestamp: Date.now(),
      })
    })
}

// AgentCommunicationService

/** Get all registered Agents */
export async function getAgents(
  filter = '',
  limit = 100,
): Promise<GetAgentsResponse> {
  return unaryCall(AGENT_COMM, 'GetAgents', {
    filter,
    limit,
    offset: 0,
  })
}

/** Find Agent by tag/skill/keyword */
export async function findAgents(
  params: Partial<FindAgentsRequest>,
): Promise<FindAgentsResponse> {
  return unaryCall(AGENT_COMM, 'FindAgents', {
    tag: '',
    skill: '',
    keyword: '',
    limit: 100,
    ...params,
  })
}

/**
 * Fetch runtime metrics for the given agent
 * RPC: AIQueryService/GetAgentMetrics
 */
export async function getAgentMetrics(
  agentId: string,
): Promise<{ data: AgentMetrics | null; error?: string }> {
  try {
    const response = await unaryCall<{ agent_id: string }, GetAgentMetricsResponse>(
      AI_QUERY,
      'GetAgentMetrics',
      { agent_id: agentId },
    )
    return { data: response.metrics ?? null }
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e)
    console.warn(`Failed to get metrics for ${agentId}:`, e)
    return { data: null, error: msg }
  }
}

// UserService (Auth)

/** User registration */
export async function register(
  username: string,
  password: string,
  displayName = '',
): Promise<RegisterResponse> {
  const req: RegisterRequest = { username, password, display_name: displayName }
  return unaryCall<RegisterRequest, RegisterResponse>(USER_AUTH, 'Register', req)
}

/** User login */
export async function login(
  username: string,
  password: string,
): Promise<LoginResponse> {
  const req: LoginRequest = { username, password }
  return unaryCall<LoginRequest, LoginResponse>(USER_AUTH, 'Login', req)
}

// ObservabilityService

/**
 * Fetch trace detail
 */
export async function getTraceDetail(traceId: string): Promise<GetTraceDetailResponse | null> {
  try {
    return await unaryCall<{ trace_id: string }, GetTraceDetailResponse>(
      OBSERVABILITY, 'GetTraceDetail', { trace_id: traceId }
    )
  } catch (e) {
    console.warn('Failed to get trace detail:', e)
    return null
  }
}

/**
 * Fetch cost report
 */
export async function getCostReport(
  userId: string,
  startDate: string,
  endDate: string
): Promise<GetCostReportResponse | null> {
  try {
    return await unaryCall<{ user_id: string; start_date: string; end_date: string }, GetCostReportResponse>(
      OBSERVABILITY, 'GetCostReport', { user_id: userId, start_date: startDate, end_date: endDate }
    )
  } catch (e) {
    console.warn('Failed to get cost report:', e)
    return null
  }
}

// OrchestrationService — ExecutePlan / Replay / Export

/** Execute a user-approved (or user-modified) DAG plan (U4). */
export async function executePlan(
  dag: DAGStructure,
  contextId: string,
): Promise<ExecutePlanResponse> {
  const req: ExecutePlanRequest = { dag, context_id: contextId, user_id: '' }
  return unaryCall<ExecutePlanRequest, ExecutePlanResponse>(ORCHESTRATION, 'ExecutePlan', req)
}

/** Replay a traced query (mode: "exact" = re-execute, "route" = route comparison) */
export async function replayQuery(
  traceId: string,
  mode: 'exact' | 'route',
): Promise<ReplayQueryResponse> {
  const req: ReplayQueryRequest = { trace_id: traceId, mode }
  return unaryCall<ReplayQueryRequest, ReplayQueryResponse>(ORCHESTRATION, 'ReplayQuery', req)
}

/** Export a conversation as Markdown or HTML (file_data is base64) */
export async function exportConversation(
  contextId: string,
  format: 'markdown' | 'html',
): Promise<ExportConversationResponse> {
  const req: ExportConversationRequest = { context_id: contextId, format }
  return unaryCall<ExportConversationRequest, ExportConversationResponse>(
    ORCHESTRATION, 'ExportConversation', req,
  )
}

// SharingService — Share / ReadSharedConversation / Templates

/** Create a read-only share link; the raw token is returned exactly once */
export async function shareSession(
  contextId: string,
  expiryDays = 0,
): Promise<ShareSessionResponse> {
  const req: ShareSessionRequest = { context_id: contextId, mode: 'view', expiry_days: expiryDays }
  return unaryCall<ShareSessionRequest, ShareSessionResponse>(SHARING, 'ShareSession', req)
}

/** Public (no auth) read of a shared conversation by raw token */
export async function readSharedConversation(
  token: string,
): Promise<ReadSharedConversationResponse> {
  const req: ReadSharedConversationRequest = { token }
  return unaryCall<ReadSharedConversationRequest, ReadSharedConversationResponse>(
    SHARING, 'ReadSharedConversation', req,
  )
}

/** List the authenticated owner's shares */
export async function listShares(): Promise<ListSharesResponse> {
  return unaryCall<object, ListSharesResponse>(SHARING, 'ListShares', {})
}

/** Revoke one of the authenticated owner's shares */
export async function revokeShare(shareId: string): Promise<RevokeShareResponse> {
  return unaryCall<{ share_id: string }, RevokeShareResponse>(
    SHARING, 'RevokeShare', { share_id: shareId },
  )
}

/** List the authenticated owner's templates */
export async function listTemplates(): Promise<ListTemplatesResponse> {
  return unaryCall<object, ListTemplatesResponse>(SHARING, 'ListTemplates', {})
}

/** Get one of the authenticated owner's templates */
export async function getTemplate(templateId: string): Promise<GetTemplateResponse> {
  return unaryCall<{ template_id: string }, GetTemplateResponse>(
    SHARING, 'GetTemplate', { template_id: templateId },
  )
}

/** Save a template (dagJson must be valid JSON, validated server-side) */
export async function saveTemplate(
  name: string,
  description: string,
  dagJson: string,
): Promise<SaveTemplateResponse> {
  const req: SaveTemplateRequest = { name, description, dag_json: dagJson }
  return unaryCall<SaveTemplateRequest, SaveTemplateResponse>(SHARING, 'SaveTemplate', req)
}

/** Use a template to create a real conversation under the current owner */
export async function useTemplate(templateId: string): Promise<UseTemplateResponse> {
  return unaryCall<{ template_id: string }, UseTemplateResponse>(
    SHARING, 'UseTemplate', { template_id: templateId },
  )
}

// UserExperienceService — Sandbox / Intervention

/**
 * Run a sandbox query through the durable pipeline. When the owner's
 * autonomy level requires confirmation, the backend creates a pending
 * intervention instead of executing (intervention_required = true).
 */
export async function sandboxQuery(
  agentId: string,
  queryText: string,
): Promise<SandboxQueryResponse> {
  const req: SandboxQueryRequest = { agent_id: agentId, query_text: queryText }
  return unaryCall<SandboxQueryRequest, SandboxQueryResponse>(
    USER_EXPERIENCE, 'SandboxQuery', req,
  )
}

/** Resolve a pending intervention (PROCEED / MODIFY / SKIP / ABORT) */
export async function interventionResponse(
  interventionId: string,
  decision: 'PROCEED' | 'MODIFY' | 'SKIP' | 'ABORT',
  modificationText?: string,
): Promise<InterventionResponseResponse> {
  const req: InterventionResponseRequest = {
    intervention_id: interventionId,
    decision,
    modification_text: modificationText,
  }
  return unaryCall<InterventionResponseRequest, InterventionResponseResponse>(
    USER_EXPERIENCE, 'InterventionResponse', req,
  )
}

// AgentLifecycleService — Compare / Autonomy / Undo

/** Compare at most 3 agents running the same question in parallel */
export async function compareAgents(
  question: string,
  agentIds: string[],
): Promise<CompareAgentsResponse> {
  const req: CompareAgentsRequest = { question, agent_ids: agentIds }
  return unaryCall<CompareAgentsRequest, CompareAgentsResponse>(
    AGENT_LIFECYCLE, 'CompareAgents', req,
  )
}

/** List the owner's persisted compare runs (true empty state when none) */
export async function getAgentCompare(): Promise<GetAgentCompareResponse> {
  return unaryCall<GetAgentCompareRequest, GetAgentCompareResponse>(
    AGENT_LIFECYCLE, 'GetAgentCompare', {},
  )
}

/** Set the owner's autonomy level (1..4) for one agent */
export async function setAutonomyLevel(
  agentId: string,
  level: number,
): Promise<SetAutonomyLevelResponse> {
  const req: SetAutonomyLevelRequest = { agent_id: agentId, level }
  return unaryCall<SetAutonomyLevelRequest, SetAutonomyLevelResponse>(
    AGENT_LIFECYCLE, 'SetAutonomyLevel', req,
  )
}

/** Undo a reversible action by its undo_actions id */
export async function undoAction(actionId: string): Promise<UndoActionResponse> {
  const req: UndoActionRequest = { action_id: actionId }
  return unaryCall<UndoActionRequest, UndoActionResponse>(
    AGENT_LIFECYCLE, 'UndoAction', req,
  )
}
