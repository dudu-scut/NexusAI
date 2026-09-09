import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { queryStream } from '../services/grpc-client'
import type { ChatMessage, AIStreamEvent, ActivityEntry } from '../types/proto'

export const useChatStore = defineStore('chat', () => {
  const messages = ref<ChatMessage[]>([])
  const isStreaming = ref(false)
  const contextId = ref(generateContextId())
  const abortController = ref<AbortController | null>(null)
  const activityEntries = ref<ActivityEntry[]>([])

  const lastAgentName = computed(() => {
    for (let i = messages.value.length - 1; i >= 0; i--) {
      if (messages.value[i].role === 'agent' && messages.value[i].agentName) {
        return messages.value[i].agentName
      }
    }
    return ''
  })

  function addActivity(type: ActivityEntry['type'], message: string, extra?: Partial<ActivityEntry>) {
    activityEntries.value = [...activityEntries.value, {
      timestamp: Date.now(),
      type,
      message,
      ...extra,
    }]
    if (activityEntries.value.length > 100) {
      activityEntries.value = activityEntries.value.slice(-100)
    }
  }

  // Returns whether the question was accepted (queued for streaming).
  // deep-review fr-R5: a rejection (stream busy / awaiting plan confirmation)
  // must be visible to the caller so the UI keeps the text and tells the
  // user — previously the send was silently swallowed while the input box
  // was cleared and a fake "Sending query" activity entry was logged.
  function sendQuestion(text: string, planOnly = false): boolean {
    if (isStreaming.value || !text.trim()) return false
    // B1: a delivered plan awaits confirmation — block new questions so
    // "confirm" cannot accidentally execute the wrong (newer) message's plan.
    if (messages.value.some((m) => m.awaitingConfirmation)) return false

    messages.value.push({
      id: crypto.randomUUID(),
      role: 'user',
      content: text,
      timestamp: Date.now(),
    })

    startStream(text, planOnly)
    return true
  }

  // Shared streaming path for fresh questions and retries. planOnly drives
  // the B1/U4 two-phase mode: plan-only run → confirm → ExecutePlan.
  function startStream(text: string, planOnly = false) {
    const agentMsg: ChatMessage = {
      id: crypto.randomUUID(),
      role: 'agent',
      content: '',
      streaming: true,
      timestamp: Date.now(),
    }
    messages.value.push(agentMsg)
    const reactiveMsg = messages.value[messages.value.length - 1]
    isStreaming.value = true

    const ac = new AbortController()
    abortController.value = ac

    addActivity('thinking', 'Analyzing request...')

    queryStream(
      text,
      (event: AIStreamEvent) => handleStreamEvent(event, reactiveMsg),
      contextId.value,
      ac.signal,
      planOnly,
    ).finally(() => {
      if (reactiveMsg.streaming) {
        reactiveMsg.streaming = false
        reactiveMsg.content += '\n[Connection lost]'
        // Mark the message failed so the retry bar renders and retryLast()
        // can drop this bubble before re-running (previously the connection
        // loss left no retry affordance and stale bubbles stacked up).
        reactiveMsg.error = 'Connection lost'
        addActivity('error', 'Connection unexpectedly closed')
      }
      isStreaming.value = false
      abortController.value = null
    })
  }

  // Retry the last user question after a real failure. Drops trailing
  // errored agent placeholders and re-runs the same durable pipeline.
  function retryLast() {
    if (isStreaming.value) return
    let lastUser: ChatMessage | undefined
    for (let i = messages.value.length - 1; i >= 0; i--) {
      if (messages.value[i].role === 'user') {
        lastUser = messages.value[i]
        break
      }
    }
    if (!lastUser) return
    while (messages.value.length) {
      const tail = messages.value[messages.value.length - 1]
      if (tail.role === 'agent' && tail.error) {
        messages.value.pop()
      } else {
        break
      }
    }
    startStream(lastUser.content)
  }

  function handleStreamEvent(event: AIStreamEvent, msg: ChatMessage) {
    switch (event.event_type) {
      case 'partial':
        if (msg.content === 'Analyzing request...') {
          msg.content = ''
        }
        msg.content += event.content
        break

      case 'status':
        if (event.task_state === 'planning') {
          if (!msg.content) {
            msg.content = 'Analyzing request...'
          }
          addActivity('thinking', 'Planning tasks...')
        } else if (event.content === 'awaiting_confirmation') {
          // B1/U4 plan-only run: the plan above awaits user confirmation.
          // The stream ends right after this marker with no terminal event,
          // so close the streaming state HERE — otherwise the finally block
          // misreads the EOF as a connection loss (P2-6).
          if (msg.executionPlan) {
            msg.awaitingConfirmation = true
            addActivity('thinking', 'Plan ready — awaiting confirmation')
          } else {
            // Malformed plan JSON above: surface it instead of silently
            // entering a confirmation flow with nothing to confirm.
            msg.error = 'Plan delivery failed (malformed plan payload)'
            addActivity('error', msg.error)
          }
          msg.streaming = false
          isStreaming.value = false
          abortController.value = null
        } else if (event.content && event.content !== 'thinking') {
          // Status contents are intents/descriptions, not agent names —
          // agent attribution arrives via the plan / subtask events.
          addActivity('thinking', event.content)
        }
        break

      case 'plan':
        try {
          const plan = JSON.parse(event.content)
          msg.executionPlan = {
            original_query: plan.original_query,
            tasks: (plan.tasks || []).map((t: any) => ({
              id: t.id,
              description: t.description,
              skill: t.skill,
              depends_on: t.depends_on || [],
              status: 'pending' as const,
              agent_id: t.agent_id,
              agent_name: t.agent_name,
              // P12(a): routing-candidate provenance rides the plan JSON;
              // embedding confidence is a real cosine similarity while
              // ranking values are labelled placeholders in the UI.
              candidates: t.candidates || undefined,
            })),
          }
          addActivity('thinking', `Execution plan: ${plan.tasks?.length || 0} subtask(s)`)
        } catch {
          // Malformed plan JSON: surface it — the awaiting_confirmation
          // marker checks executionPlan, so the confirm bar never appears
          // for an undeliverable plan.
          msg.error = 'Plan delivery failed (malformed plan payload)'
          addActivity('error', msg.error)
        }
        break

      case 'subtask_start':
        if (msg.executionPlan) {
          const task = msg.executionPlan.tasks.find(t => t.id === event.task_state)
          if (task) {
            task.status = 'running'
            if (task.agent_id) {
              msg.agentName = task.agent_name || task.agent_id
            }
            addActivity('tool_call', `Executing subtask: ${task.description}`, {
              agent_name: task.agent_id,
              tool_name: task.skill,
            })
          }
        }
        break

      case 'subtask_complete':
        if (msg.executionPlan) {
          const task = msg.executionPlan.tasks.find(t => t.id === event.task_state)
          if (task) {
            task.status = event.content?.startsWith('FAILED:') ? 'failed' : 'completed'
            task.result = event.content
            addActivity(
              task.status === 'completed' ? 'complete' : 'error',
              `${task.status === 'completed' ? 'Completed' : 'Failed'}: ${task.description}`
            )
          }
        }
        break

      case 'complete':
        msg.streaming = false
        // awaitingConfirmation is intentionally NOT cleared here: the
        // plan-only confirm/abandon flow owns that flag (the proxy no
        // longer synthesizes a complete frame for awaiting_confirmation
        // streams — gateway contract).
        msg.processingTimeMs = Date.now() - msg.timestamp
        // deep-review fr-*: the pipeline fills trace_summary on this event
        // (span_name Xms -> ...) and the gateway relays it verbatim — it
        // was received and dropped before. The structured TraceInfo UI has
        // no backend channel, so this text is the real trace record.
        if (event.trace_summary) {
          msg.traceSummaryText = event.trace_summary
        }
        isStreaming.value = false
        abortController.value = null
        addActivity('complete', 'Query completed', {
          duration_ms: msg.processingTimeMs,
        })
        break

      case 'error':
        // Structured stream errors carry code semantics in content
        // ("CODE_NAME: details"), so content is the primary source; the
        // optional details field is only a fallback when content is empty.
        msg.error = event.content || (event as AIStreamEvent & { details?: string }).details || 'Unknown error'
        msg.streaming = false
        msg.awaitingConfirmation = false
        isStreaming.value = false
        abortController.value = null
        addActivity('error', `Error: ${msg.error}`)
        break
    }
  }

  function setFeedback(msgId: string, type: 'like' | 'dislike') {
    const msg = messages.value.find(m => m.id === msgId)
    if (msg) {
      msg.feedbackGiven = msg.feedbackGiven === type ? null : type
    }
  }

  function stopStreaming() {
    if (abortController.value) {
      abortController.value.abort()
      abortController.value = null
    }
    isStreaming.value = false
    for (let i = messages.value.length - 1; i >= 0; i--) {
      const msg = messages.value[i]
      if (msg.role === 'agent' && msg.streaming) {
        msg.streaming = false
        msg.content += '\n[Stopped]'
        break
      }
    }
  }

  function newConversation() {
    stopStreaming()
    messages.value = []
    contextId.value = generateContextId()
    activityEntries.value = []
  }

  return {
    messages,
    isStreaming,
    contextId,
    lastAgentName,
    activityEntries,
    sendQuestion,
    retryLast,
    stopStreaming,
    newConversation,
    setFeedback,
    addActivity,
  }
})

function generateContextId(): string {
  return 'ctx-' + crypto.randomUUID().slice(0, 8)
}
