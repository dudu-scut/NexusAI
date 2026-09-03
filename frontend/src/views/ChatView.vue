<template>
  <div class="chat-view">
    <!-- Decorative background blobs -->
    <div class="bg-blob blob-1"></div>
    <div class="bg-blob blob-2"></div>
    <div class="bg-blob blob-3"></div>

    <div class="chat-main">
      <div class="chat-header glass">
        <div class="header-brand">
          <div class="brand-icon">
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/><line x1="12" y1="22" x2="12" y2="15.5"/><polyline points="22 8.5 12 15.5 2 8.5"/>
            </svg>
          </div>
          <h1 class="gradient-text">NexusAI</h1>
        </div>
        <div class="header-actions">
          <button class="btn-icon" @click="showActivityPanel = !showActivityPanel" :class="{ active: showActivityPanel }" title="Activity Log">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/>
              <line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/><polyline points="10 9 9 9 8 9"/>
            </svg>
          </button>
          <button class="btn-icon" @click="exportMarkdown" title="Export Markdown">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>
            </svg>
          </button>
          <button class="btn-icon" @click="handleShare" :disabled="sharing" title="Share Conversation">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/><line x1="8.59" y1="13.51" x2="15.42" y2="17.49"/><line x1="15.41" y1="6.51" x2="8.59" y2="10.49"/>
            </svg>
          </button>
          <button class="btn-text" @click="chatStore.newConversation()">New Chat</button>
          <router-link v-if="authStore.isAdmin" to="/admin" class="btn-text">Admin</router-link>
          <button class="btn-text btn-logout" @click="handleLogout">Logout</button>
        </div>
      </div>

      <div class="chat-messages" ref="messagesRef">
        <div v-if="chatStore.messages.length === 0" class="empty-state">
          <div class="empty-hero">
            <div class="empty-brand-wrapper">
              <div class="brand-glow"></div>
              <h2 class="empty-brand gradient-text">NexusAI</h2>
            </div>
            <p class="empty-tagline">Intelligent Multi-Agent Collaboration Platform</p>
            <p class="empty-subtitle">AI auto-routes to the best Agent, supporting multi-agent collaboration for complex tasks</p>
          </div>

          <div class="feature-grid">
            <div class="feature-card" style="--delay: 0ms">
              <div class="feature-icon" style="--icon-color: var(--brand-primary)">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/></svg>
              </div>
              <div class="feature-text">
                <span class="feature-title">Smart Routing</span>
                <span class="feature-desc">Auto-selects the best agent for your task</span>
              </div>
            </div>
            <div class="feature-card" style="--delay: 80ms">
              <div class="feature-icon" style="--icon-color: var(--color-success)">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M23 21v-2a4 4 0 0 0-3-3.87"/><path d="M16 3.13a4 4 0 0 1 0 7.75"/></svg>
              </div>
              <div class="feature-text">
                <span class="feature-title">Multi-Agent</span>
                <span class="feature-desc">Orchestrate multiple agents in parallel</span>
              </div>
            </div>
            <div class="feature-card" style="--delay: 160ms">
              <div class="feature-icon" style="--icon-color: var(--color-warning)">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/></svg>
              </div>
              <div class="feature-text">
                <span class="feature-title">Token Tracking</span>
                <span class="feature-desc">Real-time cost & token consumption</span>
              </div>
            </div>
            <div class="feature-card" style="--delay: 240ms">
              <div class="feature-icon" style="--icon-color: var(--color-info)">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
              </div>
              <div class="feature-text">
                <span class="feature-title">Live Monitoring</span>
                <span class="feature-desc">Activity feed & execution DAG</span>
              </div>
            </div>
          </div>

          <div class="quick-prompts">
            <button
              v-for="prompt in quickPrompts"
              :key="prompt"
              class="prompt-chip glass glass-hover"
              @click="quickSend(prompt)"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="prompt-spark"><path d="M12 3v1m0 16v1m-8-9H3m18 0h-1m-2.636-6.364l-.707.707M6.343 17.657l-.707.707m0-12.728l.707.707m11.314 11.314l.707.707"/></svg>
              {{ prompt }}
            </button>
          </div>
        </div>

        <TransitionGroup name="message" tag="div" class="message-list">
          <MessageBubble
            v-for="msg in chatStore.messages"
            :key="msg.id"
            :message="msg"
            @feedback="handleFeedback"
          />
        </TransitionGroup>
      </div>

      <!-- Failure banner with retry — error reason stays visible -->
      <div v-if="lastErrorMessage && !chatStore.isStreaming" class="retry-bar">
        <span class="retry-reason">请求失败：{{ lastErrorMessage }}</span>
        <button class="btn-text retry-btn" @click="chatStore.retryLast()">重试</button>
      </div>

      <!-- P15 P1(e/f): rerun the last rendered execution plan (read-only
           confirmation) + P1(f) phase-1 edit panel (agent dropdown +
           editable descriptions; dependency editing is phase 2) -->
      <div
        v-if="lastExecutionPlan && !chatStore.isStreaming && !lastErrorMessage"
        class="retry-bar"
      >
        <span class="retry-reason">已完成计划：{{ lastExecutionPlan.tasks.length }} 个子任务</span>
        <button class="btn-text retry-btn" :disabled="rerunningPlan" @click="rerunLastPlan">
          {{ rerunningPlan ? '重跑中…' : '重跑此计划' }}
        </button>
        <button class="btn-text retry-btn" :disabled="rerunningPlan" @click="openPlanEditor">
          {{ editingPlan ? '收起编辑' : '编辑计划' }}
        </button>
      </div>

      <div v-if="editingPlan" class="plan-editor glass">
        <div v-for="task in editedTasks" :key="task.id" class="plan-editor-row">
          <span class="plan-task-id">{{ task.id }}</span>
          <textarea
            v-model="task.description"
            class="plan-desc-input"
            rows="2"
          />
          <select v-model="task.agent_id" class="plan-agent-select">
            <option value="">自动路由</option>
            <option
              v-for="agent in availableAgents"
              :key="agent.service_name"
              :value="agent.service_name"
            >
              {{ agent.service_name }}
            </option>
          </select>
        </div>
        <button class="btn-send" :disabled="submittingPlan" @click="submitEditedPlan">
          {{ submittingPlan ? '执行中…' : '执行编辑后的计划' }}
        </button>
      </div>

      <div class="chat-input-area">
        <div class="input-wrapper glass" :class="{ 'input-focused': isFocused }">
          <textarea
            v-model="inputText"
            :placeholder="chatStore.isStreaming ? 'Agent is responding...' : 'Enter your question...'"
            :disabled="chatStore.isStreaming"
            @keydown.enter.exact.prevent="handleSend"
            @compositionstart="composing = true"
            @compositionend="composing = false"
            @focus="isFocused = true"
            @blur="isFocused = false"
            rows="1"
            @input="autoResize"
            ref="textareaRef"
          />
          <button
            v-if="chatStore.isStreaming"
            class="btn-stop"
            @click="chatStore.stopStreaming()"
          >
            <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="4" width="4" height="16"/><rect x="14" y="4" width="4" height="16"/></svg>
            Stop
          </button>
          <button
            v-else
            class="btn-send"
            :disabled="!inputText.trim()"
            @click="handleSend"
          >
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
              <line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2"/>
            </svg>
          </button>
        </div>
      </div>
    </div>

    <!-- Activity Panel Sidebar -->
    <transition name="slide">
      <div v-if="showActivityPanel" class="chat-sidebar glass">
        <ActivityPanel :entries="activityEntries" />
      </div>
    </transition>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, nextTick, watch, inject } from 'vue'
import { useRouter } from 'vue-router'
import { useChatStore } from '../stores/chat'
import { useAuthStore } from '../stores/auth'
import { shareSession } from '../services/grpc-client'
import { executePlan, getAgents } from '../services/grpc-client'
import type { DAGStructure, ServiceInfo } from '../types/proto'
import MessageBubble from '../components/MessageBubble.vue'
import ActivityPanel from '../components/ActivityPanel.vue'
import type { ActivityEntry } from '../types/proto'

const router = useRouter()
const chatStore = useChatStore()
const authStore = useAuthStore()
const toast = inject<any>('toast')
const inputText = ref('')
const composing = ref(false)
const messagesRef = ref<HTMLElement>()
const textareaRef = ref<HTMLTextAreaElement>()
const showActivityPanel = ref(false)
const isFocused = ref(false)
const sharing = ref(false)

// Last stream/unary failure shown with a retry entry (chat store retryLast).
const lastErrorMessage = computed(() => {
  const tail = chatStore.messages[chatStore.messages.length - 1]
  return tail?.role === 'agent' && tail.error ? tail.error : ''
})

// One-time share link via SharingService.ShareSession.
// The raw token is returned exactly once by the server — surface it to the
// user immediately; failures are shown as real errors, never faked.
async function handleShare() {
  if (sharing.value) return
  if (!chatStore.messages.length) {
    toast?.addToast({ type: 'error', message: '当前会话没有可分享的消息' })
    return
  }
  sharing.value = true
  try {
    const resp = await shareSession(chatStore.contextId)
    if (resp.status.code !== 0) {
      toast?.addToast({ type: 'error', message: resp.status.message || '分享创建失败' })
      return
    }
    const url = resp.share_url ? window.location.origin + resp.share_url : ''
    toast?.addToast({
      type: 'success',
      message: `分享已创建：${url || resp.share_id}（token 仅显示一次，请妥善保存）`,
    })
  } catch (e) {
    toast?.addToast({ type: 'error', message: e instanceof Error ? e.message : String(e) })
  } finally {
    sharing.value = false
  }
}

// Activity feed
const activityEntries = ref<ActivityEntry[]>([])

const quickPrompts = [
  'Explain microservices architecture',
  'Write a quicksort in Python',
  'Compare REST vs GraphQL',
  'How to design a high-availability system?',
]

function quickSend(text: string) {
  inputText.value = text
  handleSend()
}

function handleSend() {
  if (composing.value) return
  const text = inputText.value.trim()
  if (!text) return
  chatStore.sendQuestion(text)
  inputText.value = ''
  if (textareaRef.value) {
    textareaRef.value.style.height = 'auto'
  }
  // Add activity entry
  addActivity('agent_call', `Sending query: ${text.slice(0, 60)}${text.length > 60 ? '...' : ''}`)
}

function handleFeedback(msgId: string, type: 'like' | 'dislike') {
  chatStore.setFeedback(msgId, type)
  addActivity('complete', type === 'like' ? 'User marked as helpful 👍' : 'User marked as not helpful 👎')
}

function addActivity(type: ActivityEntry['type'], message: string, extra?: Partial<ActivityEntry>) {
  activityEntries.value = [...activityEntries.value, {
    timestamp: Date.now(),
    type,
    message,
    ...extra,
  }]
  // Keep last 50
  if (activityEntries.value.length > 50) {
    activityEntries.value = activityEntries.value.slice(-50)
  }
}

function handleLogout() {
  authStore.logout()
  router.push('/login')
}

function autoResize() {
  if (textareaRef.value) {
    textareaRef.value.style.height = 'auto'
    textareaRef.value.style.height = Math.min(textareaRef.value.scrollHeight, 150) + 'px'
  }
}

function exportMarkdown() {
  const msgs = chatStore.messages
  if (msgs.length === 0) return

  let md = '# NexusAI Conversation Log\n\n'
  md += `> Exported: ${new Date().toLocaleString()}\n`
  md += `> Conversation ID: ${chatStore.contextId}\n\n---\n\n`

  for (const msg of msgs) {
    const role = msg.role === 'user' ? '👤 **User**' : `🤖 **${msg.agentName || 'Agent'}**`
    md += `### ${role}\n\n`
    if (msg.traceInfo) {
      md += `> Route ${msg.traceInfo.route_time_ms}ms → ${msg.traceInfo.agent_name} ${msg.traceInfo.agent_time_ms}ms (total ${msg.traceInfo.total_time_ms}ms)\n\n`
    }
    md += `${msg.content}\n\n---\n\n`
  }

  const blob = new Blob([md], { type: 'text/markdown' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = `nexusai-${chatStore.contextId}.md`
  a.click()
  URL.revokeObjectURL(url)

  addActivity('complete', 'Conversation exported as Markdown')
  toast?.addToast({ type: 'success', message: '对话已导出为 Markdown' })
}

// Watch for streaming events to populate activity feed
watch(
  () => chatStore.messages[chatStore.messages.length - 1]?.executionPlan,
  (plan) => {
    if (plan) {
      addActivity('thinking', `Execution plan: ${plan.tasks.length} subtask(s)`)
      plan.tasks.forEach(t => {
        addActivity('thinking', `Task [${t.id}]: ${t.description}`)
      })
    }
  },
)

// P15 P1(e): rerun the last rendered execution plan — posts the plan
// verbatim (read-only confirmation form; DAG editing lands in the panel
// follow-up).
const lastExecutionPlan = computed(
  () => chatStore.messages[chatStore.messages.length - 1]?.executionPlan,
)
const rerunningPlan = ref(false)

async function rerunLastPlan() {
  const plan = lastExecutionPlan.value
  if (!plan || rerunningPlan.value) return
  rerunningPlan.value = true
  try {
    const dag: DAGStructure = {
      nodes: plan.tasks.map(t => ({
        id: t.id,
        description: t.description,
        agent_id: t.agent_id ?? '',
        dependencies: t.depends_on ?? [],
      })),
    }
    const response = await executePlan(dag, chatStore.contextId)
    addActivity('complete', `Plan re-executed: trace ${response.trace_id}`)
    toast?.addToast({ type: 'success', message: '计划已重新执行' })
  } catch (error) {
    addActivity('error', `Plan re-execution failed: ${(error as Error).message}`)
    toast?.addToast({ type: 'error', message: '计划重跑失败' })
  } finally {
    rerunningPlan.value = false
  }
}

// P15 P1(f) phase 1: plan editor — agent dropdown (registry-sourced) and
// editable descriptions; dependency add/remove is phase 2.
const editingPlan = ref(false)
const submittingPlan = ref(false)
const availableAgents = ref<ServiceInfo[]>([])
const editedTasks = ref<
  Array<{ id: string; description: string; agent_id: string; dependencies: string[] }>
>([])

async function openPlanEditor() {
  const plan = lastExecutionPlan.value
  if (!plan) return
  if (!editingPlan.value) {
    editingPlan.value = true
    editedTasks.value = plan.tasks.map(t => ({
      id: t.id,
      description: t.description,
      agent_id: t.agent_id ?? '',
      dependencies: t.depends_on ?? [],
    }))
    if (availableAgents.value.length === 0) {
      try {
        const response = await getAgents()
        availableAgents.value = response.agents
      } catch (error) {
        addActivity('error', `Agent list load failed: ${(error as Error).message}`)
      }
    }
  } else {
    editingPlan.value = false
  }
}

async function submitEditedPlan() {
  if (submittingPlan.value) return
  submittingPlan.value = true
  try {
    const dag: DAGStructure = {
      nodes: editedTasks.value.map(t => ({
        id: t.id,
        description: t.description,
        agent_id: t.agent_id,
        dependencies: t.dependencies,
      })),
    }
    const response = await executePlan(dag, chatStore.contextId)
    addActivity('complete', `Edited plan executed: trace ${response.trace_id}`)
    toast?.addToast({ type: 'success', message: '编辑后的计划已执行' })
    editingPlan.value = false
  } catch (error) {
    addActivity('error', `Edited plan execution failed: ${(error as Error).message}`)
    toast?.addToast({ type: 'error', message: '编辑计划执行失败' })
  } finally {
    submittingPlan.value = false
  }
}

// Auto-scroll
watch(
  () => chatStore.messages.length,
  () => nextTick(() => scrollToBottom()),
)

watch(
  () => chatStore.messages[chatStore.messages.length - 1]?.content,
  () => nextTick(() => scrollToBottom()),
)

function scrollToBottom() {
  if (messagesRef.value) {
    messagesRef.value.scrollTop = messagesRef.value.scrollHeight
  }
}
</script>

<style scoped>
@import "../styles/design-tokens.css";

@keyframes messageSlideIn {
  from { opacity: 0; transform: translateY(12px) scale(0.98); }
  to { opacity: 1; transform: translateY(0) scale(1); }
}

@keyframes blobFloat {
  0%, 100% { transform: translate(0, 0) scale(1); }
  33% { transform: translate(30px, -20px) scale(1.05); }
  66% { transform: translate(-20px, 15px) scale(0.95); }
}

@keyframes breathe {
  0%, 100% { opacity: 1; filter: brightness(1); }
  50% { opacity: 0.85; filter: brightness(1.15); }
}

.chat-view {
  display: flex;
  height: 100vh;
  background: var(--bg-primary);
  position: relative;
  overflow: hidden;
}

/* Decorative background blobs */
.bg-blob {
  position: absolute;
  border-radius: 50%;
  pointer-events: none;
  filter: blur(80px);
  opacity: 0.15;
  z-index: 0;
}

.blob-1 {
  width: 500px;
  height: 500px;
  background: var(--brand-primary);
  top: -100px;
  left: -100px;
  animation: blobFloat 20s ease-in-out infinite;
}

.blob-2 {
  width: 400px;
  height: 400px;
  background: var(--brand-secondary);
  bottom: -50px;
  right: -50px;
  animation: blobFloat 25s ease-in-out infinite reverse;
}

.blob-3 {
  width: 300px;
  height: 300px;
  background: var(--brand-tertiary);
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  animation: blobFloat 18s ease-in-out infinite 5s;
}

.chat-main {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-width: 0;
  position: relative;
  z-index: 1;
}

.chat-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--space-3) var(--space-6);
  border-bottom: 1px solid var(--border-subtle);
  border-radius: 0;
}

.header-brand {
  display: flex;
  align-items: center;
  gap: var(--space-3);
}

.brand-icon {
  display: flex;
  align-items: center;
  color: var(--brand-primary);
}

.header-brand h1 {
  font-size: 18px;
  font-weight: 700;
  margin: 0;
}

.header-actions {
  display: flex;
  align-items: center;
  gap: var(--space-2);
}

.btn-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  background: transparent;
  color: var(--text-secondary);
  cursor: pointer;
  transition: all var(--duration-fast) var(--ease-default);
}

.btn-icon:hover {
  background: var(--glass-bg-hover);
  color: var(--text-primary);
}

.btn-icon.active {
  background: rgba(99, 102, 241, 0.15);
  color: var(--brand-primary);
  border-color: var(--border-brand);
}

.btn-text {
  padding: var(--space-1) var(--space-3);
  border-radius: var(--radius-sm);
  font-size: 13px;
  cursor: pointer;
  text-decoration: none;
  border: 1px solid var(--border-default);
  background: var(--glass-bg);
  color: var(--text-secondary);
  transition: all var(--duration-fast) var(--ease-default);
  font-weight: 500;
}

.btn-text:hover {
  background: var(--glass-bg-hover);
  color: var(--text-primary);
  border-color: var(--border-strong);
}

.btn-logout {
  color: var(--text-tertiary);
  border-color: transparent;
}

.btn-logout:hover {
  color: var(--color-error);
}

.chat-messages {
  flex: 1;
  overflow-y: auto;
  padding: var(--space-6);
  scroll-behavior: smooth;
}

.message-list {
  display: flex;
  flex-direction: column;
}

/* Message transition animation */
.message-enter-active {
  animation: messageSlideIn 0.35s var(--ease-out);
}

.message-leave-active {
  transition: all 0.2s var(--ease-in);
}

.message-leave-to {
  opacity: 0;
  transform: translateY(-8px) scale(0.98);
}

.message-move {
  transition: transform 0.3s var(--ease-default);
}

/* Empty state */
.empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  height: 100%;
  text-align: center;
  gap: var(--space-8);
}

.empty-hero {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--space-3);
}

.empty-brand-wrapper {
  position: relative;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.brand-glow {
  position: absolute;
  width: 200px;
  height: 60px;
  background: radial-gradient(ellipse, rgba(99, 102, 241, 0.25), transparent 70%);
  filter: blur(20px);
  animation: breathe 4s ease-in-out infinite;
  pointer-events: none;
}

.empty-brand {
  font-size: 52px;
  font-weight: 800;
  letter-spacing: -1.5px;
  margin: 0;
  position: relative;
  z-index: 1;
}

.empty-tagline {
  font-size: 15px;
  font-weight: 500;
  color: var(--text-secondary);
  margin: 0;
  letter-spacing: 0.3px;
}

.empty-subtitle {
  color: var(--text-tertiary);
  font-size: 13px;
  margin: 0;
  max-width: 400px;
  line-height: 1.6;
}

/* Feature grid */
.feature-grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: var(--space-3);
  max-width: 480px;
  width: 100%;
}

.feature-card {
  display: flex;
  align-items: flex-start;
  gap: var(--space-3);
  padding: var(--space-4);
  background: var(--glass-bg);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-md);
  text-align: left;
  transition: all var(--duration-normal) var(--ease-default);
  animation: featureFadeIn 0.5s var(--ease-out) var(--delay, 0ms) both;
  cursor: default;
}

.feature-card:hover {
  background: var(--glass-bg-hover);
  border-color: var(--border-strong);
  transform: translateY(-2px);
  box-shadow: var(--shadow-md);
}

@keyframes featureFadeIn {
  from {
    opacity: 0;
    transform: translateY(12px) scale(0.96);
  }
  to {
    opacity: 1;
    transform: translateY(0) scale(1);
  }
}

.feature-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  min-width: 36px;
  border-radius: var(--radius-md);
  background: rgba(255, 255, 255, 0.04);
  color: var(--icon-color, var(--brand-primary));
  border: 1px solid rgba(255, 255, 255, 0.06);
}

.feature-text {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.feature-title {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.feature-desc {
  font-size: 11px;
  color: var(--text-tertiary);
  line-height: 1.4;
}

.quick-prompts {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-3);
  justify-content: center;
  max-width: 560px;
}

.prompt-chip {
  display: inline-flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-2) var(--space-4);
  border-radius: var(--radius-xl);
  color: var(--text-secondary);
  font-size: 13px;
  cursor: pointer;
  transition: all var(--duration-normal) var(--ease-default);
}

.prompt-spark {
  color: var(--text-tertiary);
  transition: color var(--duration-fast) var(--ease-default);
}

.prompt-chip:hover {
  color: var(--text-primary);
  transform: translateY(-2px);
  box-shadow: var(--shadow-md);
}

.prompt-chip:hover .prompt-spark {
  color: var(--brand-primary);
}

/* Input area */
.chat-input-area {
  padding: var(--space-4) var(--space-6);
  border-top: 1px solid var(--border-subtle);
}

/* Failure banner */
.retry-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-3);
  margin: 0 var(--space-6);
  padding: 8px 14px;
  border: 1px solid rgba(239, 68, 68, 0.3);
  border-radius: var(--radius-sm);
  background: rgba(239, 68, 68, 0.08);
  font-size: 13px;
  color: var(--color-error);
}

.retry-reason {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.retry-btn {
  color: var(--color-error);
  flex-shrink: 0;
}

.input-wrapper {
  display: flex;
  align-items: flex-end;
  gap: var(--space-3);
  border-radius: var(--radius-lg);
  padding: var(--space-3) var(--space-4);
  transition: all var(--duration-normal) var(--ease-default);
  border: 1px solid var(--glass-border);
  position: relative;
}

.input-wrapper::before {
  content: '';
  position: absolute;
  inset: -1px;
  border-radius: inherit;
  background: var(--brand-gradient);
  opacity: 0;
  transition: opacity var(--duration-normal) var(--ease-default);
  z-index: -1;
  -webkit-mask:
    linear-gradient(#fff 0 0) content-box,
    linear-gradient(#fff 0 0);
  -webkit-mask-composite: xor;
  mask-composite: exclude;
  padding: 1px;
  pointer-events: none;
}

.input-wrapper.input-focused {
  border-color: transparent;
  box-shadow: 0 0 0 3px rgba(99, 102, 241, 0.08);
}

.input-wrapper.input-focused::before {
  opacity: 1;
}

textarea {
  flex: 1;
  border: none;
  outline: none;
  resize: none;
  font-size: 15px;
  line-height: 1.5;
  font-family: inherit;
  min-height: 24px;
  background: transparent;
  color: var(--text-primary);
}

textarea::placeholder {
  color: var(--text-tertiary);
}

.btn-send, .btn-stop {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--space-2);
  border: none;
  cursor: pointer;
  white-space: nowrap;
  transition: all var(--duration-fast) var(--ease-default);
  flex-shrink: 0;
}

.btn-send {
  width: 40px;
  height: 40px;
  border-radius: var(--radius-full);
  background: var(--brand-gradient);
  color: #fff;
}

.btn-send:hover:not(:disabled) {
  background: var(--brand-gradient-hover);
  box-shadow: var(--shadow-glow-brand);
  transform: scale(1.05);
}

.btn-send:active:not(:disabled) {
  transform: scale(0.95);
}

.btn-send:disabled {
  opacity: 0.35;
  cursor: not-allowed;
}

.btn-stop {
  padding: var(--space-2) var(--space-4);
  border-radius: var(--radius-md);
  background: var(--color-error);
  color: #fff;
  font-size: 14px;
  font-weight: 600;
}

.btn-stop:hover {
  background: var(--color-error);
  box-shadow: var(--shadow-glow-error);
}

/* Sidebar - compatible with SideNav layout */
.chat-sidebar {
  width: 320px;
  min-width: 320px;
  border-left: 1px solid var(--border-subtle);
  padding: var(--space-4);
  overflow-y: auto;
  flex-shrink: 0;
}

.slide-enter-active,
.slide-leave-active {
  transition: all 0.25s var(--ease-default);
}

.slide-enter-from,
.slide-leave-to {
  width: 0;
  min-width: 0;
  opacity: 0;
  padding: 0;
}

/* Responsive */
@media (max-width: 1024px) {
  .chat-sidebar {
    position: fixed;
    top: 0;
    right: 0;
    bottom: 0;
    z-index: var(--z-overlay);
    width: 320px;
    box-shadow: var(--shadow-lg);
  }

  .chat-header {
    padding: var(--space-3) var(--space-4);
  }

  .chat-messages {
    padding: var(--space-4);
  }

  .chat-input-area {
    padding: var(--space-3) var(--space-4);
  }
}

@media (max-width: 768px) {
  .chat-sidebar {
    width: 100%;
  }

  .header-brand h1 {
    font-size: 16px;
  }

  .btn-text {
    display: none;
  }

  .empty-brand {
    font-size: 36px;
  }

  .empty-tagline {
    font-size: 13px;
  }

  .empty-subtitle {
    font-size: 12px;
    max-width: 320px;
  }

  .feature-grid {
    grid-template-columns: 1fr;
    max-width: 320px;
  }

  .quick-prompts {
    max-width: 100%;
  }

  .prompt-chip {
    font-size: 12px;
    padding: var(--space-2) var(--space-3);
  }
}

@media (max-width: 480px) {
  .chat-header {
    padding: var(--space-2) var(--space-3);
  }

  .chat-messages {
    padding: var(--space-3);
  }

  .chat-input-area {
    padding: var(--space-2) var(--space-3);
  }

  .input-wrapper {
    padding: var(--space-2) var(--space-3);
  }

  textarea {
    font-size: 14px;
  }

  .empty-brand {
    font-size: 28px;
  }

  .bg-blob {
    display: none;
  }
}

/* Reduced motion */
@media (prefers-reduced-motion: reduce) {
  .bg-blob {
    animation: none;
    display: none;
  }

  .empty-brand {
    animation: none;
  }

  .brand-glow {
    animation: none;
  }

  .feature-card {
    animation: none;
  }
}
</style>
