<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { Icon } from '@iconify/vue'
import { useAuthStore } from '../../stores/auth'

const router = useRouter()
const route = useRoute()
const auth = useAuthStore()

const SIDENAV_COLLAPSED_KEY = 'nexusai_sidenav_collapsed'

const collapsed = ref(false)

onMounted(() => {
  const saved = localStorage.getItem(SIDENAV_COLLAPSED_KEY)
  if (saved !== null) {
    collapsed.value = saved === 'true'
  }
})

watch(collapsed, (val) => {
  localStorage.setItem(SIDENAV_COLLAPSED_KEY, String(val))
})

function toggleCollapse() {
  collapsed.value = !collapsed.value
}

// Admin entry is only listed for ADMIN roles. This is a UX
// nicety — the router guard and the server's requireAdmin checks remain the
// authoritative boundaries.
const navItems = computed(() => {
  const items = [
    { label: '对话', icon: 'mdi:chat-processing-outline', path: '/' },
    { label: 'Agent拓扑', icon: 'mdi:graph-outline', path: '/topology' },
    { label: '数据面板', icon: 'mdi:chart-box-outline', path: '/dashboard' },
    { label: '系统监控', icon: 'mdi:monitor-dashboard', path: '/monitor' },
    { label: 'Agent沙盒', icon: 'mdi:flask-outline', path: '/sandbox' },
    { label: 'Agent对比', icon: 'mdi:compare-horizontal', path: '/compare' },
    { label: '模板市场', icon: 'mdi:store-outline', path: '/templates' },
  ]
  if (auth.isAdmin) {
    items.push({ label: '管理后台', icon: 'mdi:shield-crown-outline', path: '/admin' })
  }
  return items
})

function isActive(path: string) {
  if (path === '/') return route.path === '/'
  return route.path.startsWith(path)
}

function navigate(path: string) {
  router.push(path)
}

function handleLogout() {
  auth.logout()
  router.push('/login')
}

function userInitial(): string {
  return auth.username ? auth.username.charAt(0).toUpperCase() : 'U'
}
</script>

<template>
  <aside class="sidenav" :class="{ collapsed }">
    <!-- Brand -->
    <div class="brand-area">
      <div class="brand-icon">N</div>
      <Transition name="fade">
        <span v-if="!collapsed" class="brand-text">NexusAI</span>
      </Transition>
    </div>

    <!-- Navigation -->
    <nav class="nav-list">
      <div
        v-for="item in navItems"
        :key="item.path"
        class="nav-item"
        :class="{ active: isActive(item.path) }"
        @click="navigate(item.path)"
      >
        <Icon :icon="item.icon" :width="22" class="nav-icon" />
        <Transition name="fade">
          <span v-if="!collapsed" class="nav-label">{{ item.label }}</span>
        </Transition>
      </div>

      <!-- Mobile logout (deep-review fr-*): the desktop user-area is hidden
           below 768px, which left ChatView's header button as the only
           reachable logout on the other seven pages. -->
      <div class="nav-item mobile-logout" @click="handleLogout" title="登出">
        <Icon icon="mdi:logout" :width="22" class="nav-icon" />
        <Transition name="fade">
          <span v-if="!collapsed" class="nav-label">登出</span>
        </Transition>
      </div>
    </nav>

    <!-- Bottom section -->
    <div class="bottom-section">
      <!-- User info -->
      <div class="user-area">
        <div class="user-avatar">{{ userInitial() }}</div>
        <Transition name="fade">
          <div v-if="!collapsed" class="user-info">
            <span class="user-name">{{ auth.username || 'User' }}</span>
            <button class="logout-btn" @click="handleLogout" title="登出">
              <Icon icon="mdi:logout" :width="18" />
            </button>
          </div>
        </Transition>
      </div>

      <!-- Collapse toggle -->
      <button class="collapse-btn" @click="toggleCollapse" :title="collapsed ? '展开' : '折叠'">
        <Icon
          :icon="collapsed ? 'mdi:chevron-right' : 'mdi:chevron-left'"
          :width="20"
        />
        <Transition name="fade">
          <span v-if="!collapsed">收起侧栏</span>
        </Transition>
      </button>
    </div>
  </aside>
</template>

<style scoped>
@import "../../styles/design-tokens.css";

.sidenav {
  width: 240px;
  min-height: 100vh;
  display: flex;
  flex-direction: column;
  background: var(--glass-bg);
  backdrop-filter: blur(var(--glass-blur));
  -webkit-backdrop-filter: blur(var(--glass-blur));
  border-right: 1px solid var(--glass-border);
  transition: width var(--duration-normal) var(--ease-default);
  position: relative;
  z-index: var(--z-sticky);
  overflow: hidden;
}

.sidenav.collapsed {
  width: 68px;
}

/* Brand */
.brand-area {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: var(--space-6) var(--space-4);
  border-bottom: 1px solid var(--border-subtle);
  min-height: 72px;
}

.brand-icon {
  width: 36px;
  height: 36px;
  min-width: 36px;
  border-radius: var(--radius-md);
  background: var(--brand-gradient);
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 700;
  font-size: 18px;
  color: #fff;
  position: relative;
  overflow: hidden;
}

.brand-icon::after {
  content: '';
  position: absolute;
  inset: 0;
  background: linear-gradient(135deg, rgba(255,255,255,0.15), transparent 50%);
  pointer-events: none;
}

.brand-text {
  font-size: 20px;
  font-weight: 700;
  background: var(--brand-gradient);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
  white-space: nowrap;
}

/* Navigation */
.nav-list {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: var(--space-1);
  padding: var(--space-4) var(--space-2);
  overflow-y: auto;
  overflow-x: hidden;
}

.nav-item {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: var(--space-3) var(--space-3);
  border-radius: var(--radius-md);
  color: var(--text-secondary);
  cursor: pointer;
  transition: all var(--duration-normal) var(--ease-default);
  white-space: nowrap;
  position: relative;
  user-select: none;
}

.nav-item::before {
  content: '';
  position: absolute;
  left: 0; top: 50%;
  transform: translateY(-50%);
  width: 3px; height: 0;
  border-radius: 0 2px 2px 0;
  background: var(--brand-gradient);
  transition: height var(--duration-normal) var(--ease-bounce);
}

.nav-item::after {
  content: '';
  position: absolute;
  inset: 0;
  border-radius: inherit;
  opacity: 0;
  background: radial-gradient(circle at var(--mouse-x, 50%) var(--mouse-y, 50%), rgba(99, 102, 241, 0.08), transparent 60%);
  transition: opacity var(--duration-fast) var(--ease-default);
  pointer-events: none;
}

.nav-item:hover {
  background: var(--glass-bg-hover);
  color: var(--text-primary);
}

.nav-item:hover::after {
  opacity: 1;
}

.nav-item.active {
  background: rgba(99, 102, 241, 0.1);
  color: var(--brand-primary);
}

.nav-item.active::before {
  height: 60%;
}

.nav-item.active::after {
  opacity: 0;
}

.nav-item.active .nav-icon {
  filter: drop-shadow(0 0 8px rgba(99, 102, 241, 0.5));
}

.nav-icon {
  min-width: 22px;
  transition: filter var(--duration-normal) var(--ease-default);
}

.nav-label {
  font-size: 14px;
  font-weight: 500;
}

/* Bottom */
.bottom-section {
  padding: var(--space-3);
  border-top: 1px solid var(--border-subtle);
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
}

.user-area {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: var(--space-2);
  border-radius: var(--radius-md);
}

.user-avatar {
  width: 32px;
  height: 32px;
  min-width: 32px;
  border-radius: var(--radius-full);
  background: var(--brand-gradient);
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 600;
  font-size: 14px;
  color: #fff;
  position: relative;
  overflow: hidden;
}

.user-avatar::after {
  content: '';
  position: absolute;
  inset: 0;
  border-radius: inherit;
  background: linear-gradient(135deg, rgba(255,255,255,0.12), transparent 50%);
  pointer-events: none;
}

.user-info {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex: 1;
  min-width: 0;
}

.user-name {
  font-size: 13px;
  color: var(--text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.logout-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  border: none;
  border-radius: var(--radius-sm);
  background: transparent;
  color: var(--text-tertiary);
  cursor: pointer;
  transition: all var(--duration-fast) var(--ease-default);
}

.logout-btn:hover {
  background: rgba(239, 68, 68, 0.15);
  color: var(--color-error);
}

.collapse-btn {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-2) var(--space-3);
  border: none;
  border-radius: var(--radius-md);
  background: transparent;
  color: var(--text-tertiary);
  cursor: pointer;
  font-size: 13px;
  transition: all var(--duration-fast) var(--ease-default);
  white-space: nowrap;
}

.collapse-btn:hover {
  background: var(--glass-bg-hover);
  color: var(--text-primary);
}

/* Fade transition */
.fade-enter-active,
.fade-leave-active {
  transition: opacity var(--duration-normal) var(--ease-default);
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

/* Mobile: bottom tab bar */
@media (max-width: 768px) {
  .sidenav {
    position: fixed;
    bottom: 0;
    left: 0;
    right: 0;
    top: auto;
    width: 100%;
    min-height: auto;
    height: 56px;
    flex-direction: row;
    border-right: none;
    border-top: 1px solid var(--glass-border);
    z-index: var(--z-sticky);
    overflow: visible;
    padding: 0;
  }

  .sidenav.collapsed {
    width: 100%;
  }

  .brand-area {
    display: none;
  }

  .nav-list {
    flex: 1;
    flex-direction: row;
    justify-content: space-around;
    align-items: center;
    gap: 0;
    padding: 0 var(--space-2);
    overflow: visible;
  }

  .nav-item {
    flex-direction: column;
    gap: 2px;
    padding: var(--space-1) var(--space-2);
    font-size: 10px;
  }

  .nav-item::before {
    left: 50%; top: auto; bottom: 0;
    transform: translateX(-50%);
    width: 0; height: 2px;
    border-radius: 2px 2px 0 0;
  }

  .nav-item.active::before {
    width: 60%; height: 2px;
  }

  .nav-label {
    font-size: 10px;
  }

  .bottom-section {
    display: none;
  }
}

/* Mobile logout entry: hidden on desktop, appended to the bottom tab bar. */
.mobile-logout {
  display: none;
}

@media (max-width: 768px) {
  .mobile-logout {
    display: flex;
    flex-direction: column;
    gap: 2px;
    padding: var(--space-1) var(--space-2);
    font-size: 10px;
    color: var(--text-muted);
  }
}
</style>
