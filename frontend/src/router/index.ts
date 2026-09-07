import { createRouter, createWebHistory } from 'vue-router'
import { useAuthStore } from '../stores/auth'

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes: [
    {
      path: '/login',
      name: 'login',
      component: () => import('../views/LoginView.vue'),
      meta: { title: '登录', requiresAuth: false, public: true },
    },
    {
      path: '/',
      name: 'chat',
      component: () => import('../views/ChatView.vue'),
      meta: { title: '对话', icon: 'mdi:chat-processing-outline', requiresAuth: true },
    },
    {
      path: '/topology',
      name: 'topology',
      component: () => import('../views/AgentTopology.vue'),
      meta: { title: 'Agent拓扑', icon: 'mdi:graph-outline', requiresAuth: true },
    },
    {
      path: '/admin',
      name: 'admin',
      component: () => import('../views/AdminView.vue'),
      meta: { title: '管理后台', icon: 'mdi:shield-crown-outline', requiresAuth: true, requiresAdmin: true },
    },
    {
      path: '/sandbox',
      name: 'sandbox',
      component: () => import('../views/AgentSandbox.vue'),
      meta: { title: 'Agent沙盒', icon: 'mdi:flask-outline', requiresAuth: true },
    },
    {
      path: '/compare',
      name: 'compare',
      component: () => import('../views/CompareView.vue'),
      meta: { title: 'Agent对比', icon: 'mdi:compare-horizontal', requiresAuth: true },
    },
    {
      path: '/share/:shareId',
      name: 'share',
      component: () => import('../views/ShareView.vue'),
      meta: { title: '分享', requiresAuth: false, public: true },
    },
    {
      path: '/monitor',
      name: 'monitor',
      component: () => import('../views/Monitor.vue'),
      meta: { title: '系统监控', icon: 'mdi:monitor-dashboard', requiresAuth: true },
    },
    {
      path: '/templates',
      name: 'templates',
      component: () => import('../views/TemplateMarket.vue'),
      meta: { title: '模板市场', icon: 'mdi:store-outline', requiresAuth: true },
    },
    {
      path: '/dashboard',
      name: 'dashboard',
      component: () => import('../views/Dashboard.vue'),
      meta: { title: '数据面板', icon: 'mdi:chart-box-outline', requiresAuth: true },
    },
    {
      path: '/:pathMatch(.*)*',
      redirect: '/login',
    },
  ],
})

router.beforeEach((to) => {
  if (to.meta.public) {
    const auth = useAuthStore()
    if (to.name === 'login' && auth.isAuthenticated) {
      return { name: 'chat' }
    }
    return true
  }

  const auth = useAuthStore()
  if (!auth.isAuthenticated) {
    // Passive cleanup: the token is expired/missing, so no Logout RPC is
    // sent (nothing to revoke server-side).
    if (auth.token) auth._clearLocalOnly()
    return { name: 'login' }
  }
  // Client-side admin gate is a UX nicety only — the server enforces
  // admin checks on every admin RPC regardless.
  if (to.meta.requiresAdmin && !auth.isAdmin) {
    return { name: 'chat' }
  }
  return true
})

export default router
