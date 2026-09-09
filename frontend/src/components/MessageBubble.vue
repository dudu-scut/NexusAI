<template>
  <div class="message" :class="message.role">
    <!-- Agent avatar for AI messages -->
    <div v-if="message.role === 'agent'" class="agent-avatar" :style="avatarStyle">
      {{ avatarLetter }}
    </div>

    <div class="message-body">
      <div v-if="message.role === 'agent'" class="message-meta">
        <AgentBadge v-if="message.agentName" :name="message.agentName" />
        <span v-if="message.processingTimeMs" class="time">
          {{ formatTime(message.processingTimeMs) }}
        </span>
      </div>

      <div ref="contentRef" class="message-content" :class="{ 'has-error': message.error }">
        <ExecutionPlan v-if="message.executionPlan" :plan="message.executionPlan" />
        <template v-if="message.error">
          <span class="error-icon">!</span>
          {{ message.error }}
        </template>
        <template v-else-if="message.streaming">
          <StreamingText v-if="message.content" :text="message.content" />
          <div v-else class="typing-indicator">
            <span class="dot"></span><span class="dot"></span><span class="dot"></span>
          </div>
        </template>
        <template v-else>
          {{ message.content }}
        </template>
      </div>

      <!-- Trace summary: the durable pipeline's human-readable record
           (span_name Xms -> ...) relayed on the complete event. The former
           structured route/agent/total badges read TraceInfo fields that no
           backend channel ever produced. -->
      <div v-if="message.traceSummaryText" class="trace-summary">
        <span class="trace-badge">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/></svg>
          Trace
        </span>
        <span class="trace-total" :title="message.traceSummaryText">{{ message.traceSummaryText }}</span>
      </div>

      <!-- Feedback buttons -->
      <div v-if="message.role === 'agent' && !message.streaming && !message.error" class="feedback-row">
        <button
          class="feedback-btn like-btn"
          :class="{ active: message.feedbackGiven === 'like' }"
          @click="$emit('feedback', message.id, 'like')"
          title="Helpful"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" :fill="message.feedbackGiven === 'like' ? 'currentColor' : 'none'" stroke="currentColor" stroke-width="2">
            <path d="M14 9V5a3 3 0 0 0-3-3l-4 9v11h11.28a2 2 0 0 0 2-1.7l1.38-9a2 2 0 0 0-2-2.3H14z"/>
          </svg>
        </button>
        <button
          class="feedback-btn dislike-btn"
          :class="{ active: message.feedbackGiven === 'dislike' }"
          @click="$emit('feedback', message.id, 'dislike')"
          title="Not helpful"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" :fill="message.feedbackGiven === 'dislike' ? 'currentColor' : 'none'" stroke="currentColor" stroke-width="2">
            <path d="M10 15v4a3 3 0 0 0 3 3l4-9V2H5.72a2 2 0 0 0-2 1.7l-1.38 9a2 2 0 0 0 2 2.3H10z"/>
          </svg>
        </button>
        <button class="feedback-btn copy-btn" @click="copyContent" title="Copy response">
          <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>
          </svg>
        </button>
        <span v-if="copied" class="copied-hint">Copied</span>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, inject } from 'vue'
import type { ChatMessage } from '../types/proto'
import AgentBadge from './AgentBadge.vue'
import StreamingText from './StreamingText.vue'
import ExecutionPlan from './ExecutionPlan.vue'

const props = defineProps<{ message: ChatMessage }>()
defineEmits<{ feedback: [messageId: string, type: 'like' | 'dislike'] }>()

const copied = ref(false)
const contentRef = ref<HTMLElement>()
const toast = inject<any>('toast')

const avatarLetter = computed(() => {
  const name = props.message.agentName || 'AI'
  return name.charAt(0).toUpperCase()
})

const avatarStyle = computed(() => {
  const gradients = [
    'linear-gradient(135deg, var(--brand-primary), var(--brand-secondary))',
    'linear-gradient(135deg, var(--brand-secondary), var(--brand-tertiary))',
    'linear-gradient(135deg, var(--color-info), var(--brand-primary))',
  ]
  const name = props.message.agentName || 'AI'
  const idx = name.length % gradients.length
  return { background: gradients[idx] }
})

function formatTime(ms: number): string {
  if (ms < 1000) return `${ms}ms`
  return `${(ms / 1000).toFixed(1)}s`
}

async function copyContent() {
  if (!contentRef.value) return
  try {
    await navigator.clipboard.writeText(contentRef.value.innerText)
    copied.value = true
    toast?.addToast({ type: 'success', message: '已复制到剪贴板' })
    setTimeout(() => { copied.value = false }, 2000)
  } catch {
    toast?.addToast({ type: 'error', message: '复制失败：剪贴板权限被拒绝' })
  }
}
</script>

<style scoped>
@import "../styles/design-tokens.css";

.message {
  display: flex;
  margin-bottom: var(--space-5);
  max-width: 85%;
  gap: var(--space-3);
}

.message.user {
  margin-left: auto;
  flex-direction: row-reverse;
}

.message.agent {
  margin-right: auto;
}

/* Agent avatar */
.agent-avatar {
  width: 36px;
  height: 36px;
  min-width: 36px;
  border-radius: var(--radius-full);
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 700;
  font-size: 14px;
  color: #fff;
  flex-shrink: 0;
  align-self: flex-end;
  box-shadow: var(--shadow-sm);
  position: relative;
}

.agent-avatar::after {
  content: '';
  position: absolute;
  inset: -2px;
  border-radius: inherit;
  background: inherit;
  opacity: 0.3;
  filter: blur(6px);
  z-index: -1;
  animation: avatarGlow 3s ease-in-out infinite;
}

@keyframes avatarGlow {
  0%, 100% { opacity: 0.2; transform: scale(1); }
  50% { opacity: 0.4; transform: scale(1.05); }
}

.message-body {
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.message-meta {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  margin-bottom: var(--space-1);
}

.time {
  font-size: 12px;
  color: var(--text-tertiary);
}

.message-content {
  padding: var(--space-3) var(--space-4);
  border-radius: var(--radius-lg);
  font-size: 15px;
  line-height: 1.6;
  white-space: pre-wrap;
  word-break: break-word;
  transition: transform var(--duration-fast) var(--ease-default),
              box-shadow var(--duration-fast) var(--ease-default);
}

.message-content:hover {
  transform: translateY(-1px);
  box-shadow: var(--shadow-md);
}

/* User message: gradient bubble */
.message.user .message-content {
  background: linear-gradient(135deg, var(--brand-primary), var(--brand-secondary));
  color: #fff;
  border-bottom-right-radius: 4px;
  position: relative;
  overflow: hidden;
}

.message.user .message-content::after {
  content: '';
  position: absolute;
  top: 0;
  left: -100%;
  width: 60%;
  height: 100%;
  background: linear-gradient(90deg, transparent, rgba(255,255,255,0.08), transparent);
  transition: left 0.6s ease;
  pointer-events: none;
}

.message.user .message-content:hover::after {
  left: 120%;
}

/* Agent message: glass morphism */
.message.agent .message-content {
  background: var(--glass-bg);
  backdrop-filter: blur(var(--glass-blur));
  -webkit-backdrop-filter: blur(var(--glass-blur));
  border: 1px solid var(--glass-border);
  color: var(--text-primary);
  border-bottom-left-radius: 4px;
}

.message-content.has-error {
  background: rgba(239, 68, 68, 0.12);
  color: var(--color-error);
  border-color: rgba(239, 68, 68, 0.2);
}

.error-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--color-error);
  color: #fff;
  font-size: 12px;
  font-weight: 700;
  margin-right: var(--space-2);
}

/* Trace Summary */
.trace-summary {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  margin-top: var(--space-2);
  padding: var(--space-2) var(--space-3);
  background: var(--glass-bg);
  border: 1px solid var(--border-subtle);
  border-radius: var(--radius-sm);
  font-size: 12px;
  color: var(--text-tertiary);
  flex-wrap: wrap;
  position: relative;
  overflow: hidden;
}

.trace-summary::before {
  content: '';
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  width: 3px;
  background: var(--brand-gradient);
  border-radius: 0 2px 2px 0;
}

.trace-badge {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px var(--space-2);
  background: var(--bg-elevated);
  border: 1px solid var(--border-subtle);
  border-radius: var(--radius-full);
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--text-secondary);
}

.trace-arrow {
  color: var(--text-muted);
  font-size: 10px;
}

.trace-agent {
  font-weight: 600;
  background: var(--brand-gradient);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}

.trace-total {
  margin-left: auto;
  font-weight: 600;
  color: var(--text-primary);
}

/* Feedback Row */
.feedback-row {
  display: flex;
  align-items: center;
  gap: var(--space-1);
  margin-top: var(--space-2);
  padding-left: 2px;
}

.feedback-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  background: transparent;
  color: var(--text-tertiary);
  cursor: pointer;
  transition: all var(--duration-fast) var(--ease-default);
}

.feedback-btn:hover {
  background: var(--glass-bg-hover);
  color: var(--text-secondary);
  border-color: var(--border-subtle);
}

.feedback-btn.like-btn.active {
  background: rgba(16, 185, 129, 0.12);
  color: var(--color-success);
  border-color: rgba(16, 185, 129, 0.3);
}

.feedback-btn.dislike-btn.active {
  background: rgba(239, 68, 68, 0.12);
  color: var(--color-error);
  border-color: rgba(239, 68, 68, 0.3);
}

.copied-hint {
  font-size: 11px;
  color: var(--color-success);
  margin-left: var(--space-1);
  animation: fadeIn 0.2s ease;
}

@keyframes fadeIn {
  from { opacity: 0; transform: translateY(-4px); }
  to { opacity: 1; transform: translateY(0); }
}

/* Typing indicator */
.typing-indicator {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: var(--space-1) 0;
}

.typing-indicator .dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--text-tertiary);
  animation: typingBounce 1.2s ease-in-out infinite;
}

.typing-indicator .dot:nth-child(2) {
  animation-delay: 0.15s;
}

.typing-indicator .dot:nth-child(3) {
  animation-delay: 0.3s;
}

@keyframes typingBounce {
  0%, 60%, 100% {
    transform: translateY(0);
    opacity: 0.4;
  }
  30% {
    transform: translateY(-6px);
    opacity: 1;
  }
}
</style>
