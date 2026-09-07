/**
 * TypeScript type definitions matching protobuf messages in proto/
 * Hand-written as a replacement for protoc generation; keep in sync with proto definitions
 */

// common.proto

export interface Status {
  code: number
  message: string
  details: string
}

export interface ServiceInfo {
  service_name: string
  version: string
  host: string
  port: number
  tags: string[]
  metadata: Record<string, string>
  skills: string[]
  agent_card: string // JSON string of AgentCard
  agent_metrics?: AgentMetrics
  cacheable?: boolean
  deployment_stage?: string
  a2a_version?: string
}

// ai_query.proto

export interface AgentPreference {
  preferred_skills: string[]
  preferred_agents: string[]
  allow_fallback: boolean
}

export interface SystemContext {
  user_id: string
  user_memory: string
  conversation_history: string
  cross_agent_summary: string
  // C2 direction 4: hard facts the user stated (hints), separated from the
  // platform-inferred profile text in user_memory (proto field 5)
  user_facts?: string
}

export interface AIQueryRequest {
  request_id: string
  question: string
  context_id: string
  history_length: number
  timeout_seconds: number
  metadata: Record<string, string>
  preference?: AgentPreference
  user_id?: string
  system_context?: SystemContext
  // Sandbox execution flag (proto field 10)
  sandbox?: boolean
  // B1/U4 two-phase execution: stop after planning, finalize as "planned",
  // confirm via a follow-up ExecutePlan call (proto field 11)
  plan_only?: boolean
}

export interface AIQueryResponse {
  request_id: string
  status: Status
  answer: string
  agent_id: string
  agent_name: string
  task_id: string
  context_id: string
  processing_time_ms: number | string
  memory_hints?: Record<string, string>
  artifacts?: Artifact[]
}

export interface Artifact {
  name: string
  mime_type: string
  data: string                    // bytes → base64 (proxy sanitizeBuffers)
  metadata?: Record<string, string>
}

export interface AIStreamEvent {
  event_id: string
  event_type: 'partial' | 'status' | 'complete' | 'error' | 'plan' | 'subtask_start' | 'subtask_complete' | 'snapshot'
  content: string
  task_state: string
  context_id: string
  timestamp: number | string
  trace_summary?: string          // field 8: trace summary on 'complete' events
  activity_json?: string          // field 9: JSON activity record for feed
  intervention_required?: boolean // field 10: true when user action needed
}

// agent_service.proto

export interface GetAgentsRequest {
  filter: string
  limit: number
  offset: number
}

export interface GetAgentsResponse {
  status: Status
  agents: ServiceInfo[]
  total_count: number
}

export interface FindAgentsRequest {
  tag: string
  skill: string
  keyword: string
  limit: number
}

export interface FindAgentsResponse {
  status: Status
  agents: ServiceInfo[]
  total_count: number
}

// Agent Metrics

export interface AgentMetrics {
  agent_id: string
  success_rate: number        // 0~1
  avg_latency_ms: number
  p95_latency_ms?: number
  total_requests: number
  approval_rate?: number      // approval rate

  estimated_token_low?: number   // proto field 7
  estimated_token_high?: number  // proto field 8
}

export interface GetAgentMetricsRequest {
  agent_id: string
}

export interface GetAgentMetricsResponse {
  status: Status
  metrics: AgentMetrics
}

// Budget

export interface BudgetInfo {
  user_id: string
  daily_limit: number
  daily_used: number
  monthly_limit: number
  monthly_used: number
  remaining: number
  reset_at: number
}

// Multi-Agent Execution Plan

export interface SubTaskInfo {
  id: string
  description: string
  skill: string
  depends_on: string[]
  status: 'pending' | 'running' | 'completed' | 'failed'
  result?: string
  agent_id?: string      // backend plan event field (multi_agent_handler)
  agent_name?: string
}

export interface ExecutionPlan {
  original_query: string
  tasks: SubTaskInfo[]
}

// Activity Feed

export interface ActivityEntry {
  timestamp: number
  type: 'thinking' | 'tool_call' | 'agent_call' | 'complete' | 'error'
  message: string
  agent_name?: string
  tool_name?: string
  duration_ms?: number
  detail?: string
}

// Trace Info

export interface TraceInfo {
  trace_id: string
  route_time_ms: number
  agent_time_ms: number
  total_time_ms: number
  agent_name: string
  agent_id: string
  skill: string
}

// user.proto (auth)

export interface RegisterRequest {
  username: string
  password: string
  display_name: string
}

export interface RegisterResponse {
  status: Status
  user_id: string
  username: string
  role: string                    // USER | ADMIN
}

export interface LoginRequest {
  username: string
  password: string
}

export interface LoginResponse {
  status: Status
  user_id: string
  username: string
  token: string
  expires_at: number
  role: string                    // USER | ADMIN
}

export interface ValidateTokenRequest {
  token: string
}

export interface ValidateTokenResponse {
  status: Status
  user_id: string
  username: string
  valid: boolean
  role: string                    // USER | ADMIN
}

// LogoutRequest is intentionally empty: the session token rides the
// authorization metadata (protected endpoint, never whitelisted).
export interface LogoutRequest {
  // no fields — mirrors user.proto LogoutRequest
}

export interface LogoutResponse {
  status: Status
}

// Sharing & Templates (sharing.proto)

export interface ShareSessionRequest {
  context_id: string
  mode: string            // only "view" is supported
  expiry_days: number     // 0 = never expires
}

export interface ShareSessionResponse {
  status: Status
  share_id: string
  share_url: string       // relative path "/share/<token>"
  token: string           // raw bearer token, returned exactly once
  expires_at: string      // ISO-8601, empty = never expires
}

export interface SharedMessage {
  role: string
  content: string
  sequence_no: number | string
  created_at: string
}

export interface ReadSharedConversationRequest {
  token: string
}

export interface ReadSharedConversationResponse {
  status: Status
  title: string
  messages: SharedMessage[]
  shared_at: string
}

export interface ShareEntry {
  share_id: string
  conversation_id: string
  permission: string
  created_at: string
  expires_at: string      // empty = never expires
  revoked: boolean
  revoked_at: string
}

export interface ListSharesResponse {
  status: Status
  shares: ShareEntry[]
}

export interface RevokeShareResponse {
  status: Status
}

export interface SaveTemplateRequest {
  name: string
  description: string
  dag_json: string        // validated JSON definition
}

export interface SaveTemplateResponse {
  status: Status
  template_id: string
}

export interface TemplateEntry {
  template_id: string
  name: string
  description: string
  definition: string      // JSON definition (stored as JSONB)
  created_at: string
  version: number
}

export interface ListTemplatesResponse {
  status: Status
  templates: TemplateEntry[]
}

export interface GetTemplateResponse {
  status: Status
  template: TemplateEntry
}

export interface UseTemplateResponse {
  status: Status
  context_id: string
}

// Replay & Export (orchestration.proto)

// P15 P1(e): ExecutePlan (U4) — user-approved/modified DAG execution.
export interface DAGNode {
  id: string
  description: string
  agent_id: string
  dependencies: string[]
}

export interface DAGStructure {
  nodes: DAGNode[]
}

export interface ExecutePlanRequest {
  dag: DAGStructure
  context_id: string
  user_id: string
}

export interface ExecutePlanResponse {
  status: Status
  trace_id: string
}

export interface ReplayQueryRequest {
  trace_id: string
  mode: 'exact' | 'route'
}

export interface ReplayQueryResponse {
  status: Status
  original: string
  replayed: string
  new_trace_id: string    // empty in route mode
  new_request_id: string  // empty in route mode
}

export interface ExportConversationRequest {
  context_id: string
  format: 'markdown' | 'html'
}

export interface ExportConversationResponse {
  status: Status
  file_data: string       // base64-encoded bytes
  mime_type: string
}

// Frontend internal types

export interface ChatMessage {
  id: string
  role: 'user' | 'agent'
  content: string
  agentName?: string
  agentId?: string
  processingTimeMs?: number
  streaming?: boolean
  error?: string
  executionPlan?: ExecutionPlan
  activityFeed?: ActivityEntry[]
  traceInfo?: TraceInfo
  feedbackGiven?: 'like' | 'dislike' | null
  timestamp: number
  // B1/U4 two-phase: set when a plan-only stream ends with
  // awaiting_confirmation — ChatView renders a confirm/abandon pair that
  // drives the follow-up ExecutePlan call.
  awaitingConfirmation?: boolean
}

export interface AgentDisplayInfo {
  id: string
  name: string
  host: string
  port: number
  version: string
  tags: string[]
  skills: string[]
  healthy: boolean
  metrics?: AgentMetrics
}

// observability.proto

export interface TokenUsageRecord {
  trace_id: string
  user_id: string
  context_id: string
  agent_id: string
  component: string
  prompt_tokens: number
  completion_tokens: number
  cost_usd: number
  latency_ms: number
  created_at: string
}

export interface TraceSpan {
  trace_id: string
  span_id: string
  parent_span_id: string
  component: string
  start_time: string
  end_time: string
  duration_ms: number
  status: string
  error_message: string
  metadata_json: string
}

export interface CostRecord {
  user_id: string
  date: string           // YYYY-MM-DD
  total_cost_usd: number
  total_prompt_tokens: number
  total_completion_tokens: number
  total_requests: number
  estimated: boolean     // true when provider usage was unavailable
}

export interface GetTraceDetailResponse {
  status: Status
  spans: TraceSpan[]
  trace_summary: string
}

export interface GetCostReportResponse {
  status: Status
  records: CostRecord[]
  total_cost_usd: number
}

// Sandbox & Intervention (user_experience.proto)

export interface SandboxQueryRequest {
  query_text: string
  agent_id: string
  context_id?: string
}

export interface SandboxQueryResponse {
  status: Status
  result: string
  request_id: string           // durable pipeline request id (empty when gated)
  intervention_required: boolean
  intervention_id: string
}

export interface InterventionResponseRequest {
  trace_id?: string
  decision: 'PROCEED' | 'MODIFY' | 'SKIP' | 'ABORT'
  modification_text?: string
  intervention_id: string
}

export interface InterventionResponseResponse {
  status: Status
  new_state: string
  undo_action_id: string
  executed_request_id: string
}

// Compare / Autonomy / Undo (agent_lifecycle.proto)

export interface CompareAgentsRequest {
  question: string
  agent_ids: string[] // 1..3 entries, duplicates refused
}

export interface CompareAgentResult {
  agent_id: string
  status: string // completed | failed | cancelled
  answer: string
  error: string
  request_id: string
  trace_id: string
}

export interface CompareAgentsResponse {
  status: Status
  run_id: string
  run_status: string // completed | partial | failed | cancelled
  results: CompareAgentResult[]
}

export interface CompareRunSummary {
  run_id: string
  request_text: string
  status: string
  results_json: string
  created_at: string
}

export interface GetAgentCompareRequest {
  skill_name?: string
}

export interface GetAgentCompareResponse {
  status: Status
  agents: AgentMetrics[]
  runs: CompareRunSummary[]
}

export interface SetAutonomyLevelRequest {
  user_id?: string  // ignored — owner comes from the auth context
  agent_id: string
  level: number // 1..4
}

export interface SetAutonomyLevelResponse {
  status: Status
}

export interface UndoActionRequest {
  trace_id?: string   // legacy; superseded by action_id
  step_index?: number // legacy
  action_id: string
}

export interface UndoActionResponse {
  status: Status
  success: boolean
  message: string
}
