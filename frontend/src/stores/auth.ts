import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { login as apiLogin, register as apiRegister, logout as apiLogout, setAuthTokenGetter, setOnUnauthorized } from '../services/grpc-client'
import { useChatStore } from './chat'
import { useAgentsStore } from './agents'

const AUTH_STORAGE_KEY = 'nexusai_auth'

// Periodic token expiry check — runs once, shared across store instances
let _expiryTimer: ReturnType<typeof setInterval> | null = null
let _expiryCheckFn: (() => void) | null = null

export const useAuthStore = defineStore('auth', () => {
  const userId = ref<string | null>(null)
  const username = ref<string | null>(null)
  const token = ref<string | null>(null)
  const expiresAt = ref<number>(0)
  // Role returned by Login/ValidateToken. Used only to hide
  // admin entry points — the server enforces admin checks independently.
  const role = ref<string>('USER')

  // Hydrate from localStorage on store init
  try {
    const saved = localStorage.getItem(AUTH_STORAGE_KEY)
    if (saved) {
      const data = JSON.parse(saved)
      userId.value = data.userId ?? null
      username.value = data.username ?? null
      token.value = data.token ?? null
      expiresAt.value = data.expiresAt ?? 0
      role.value = data.role ?? 'USER'
    }
  } catch {
    // ignore corrupt localStorage data
  }

  const isAuthenticated = computed(() => {
    if (!token.value) return false
    if (Date.now() > expiresAt.value) return false
    return true
  })

  const isAdmin = computed(() => role.value === 'ADMIN')

  function saveToStorage() {
    try {
      localStorage.setItem(AUTH_STORAGE_KEY, JSON.stringify({
        userId: userId.value,
        username: username.value,
        token: token.value,
        expiresAt: expiresAt.value,
        role: role.value,
      }))
    } catch {
      // localStorage full or disabled — auth still works in-memory
    }
  }

  function setAuth(data: { user_id: string; username: string; token: string; expires_at: number; role?: string }) {
    userId.value = data.user_id
    username.value = data.username
    token.value = data.token
    expiresAt.value = data.expires_at * 1000 // server returns seconds, JS uses ms
    role.value = data.role || 'USER'
    saveToStorage()
  }

  function clearAuth() {
    userId.value = null
    username.value = null
    token.value = null
    expiresAt.value = 0
    role.value = 'USER'
    localStorage.removeItem(AUTH_STORAGE_KEY)
  }

  // Passive logout (401 response / expiry poll / route guard): local cleanup
  // ONLY — the token is already invalid/expired, so there is nothing to
  // revoke server-side and no RPC is sent.
  function _clearLocalOnly() {
    clearAuth()
    useChatStore().newConversation()
    useAgentsStore().stopPolling()
  }

  // Manual logout (user clicked logout): local cleanup + a best-effort
  // Logout RPC that revokes the server-side session (PG revoke + cache DEL
  // + deny marker). Fire-and-forget: local state is cleared first, so an
  // RPC failure must never block or delay logout — the server session dies
  // via its own 24h TTL if the revoke never lands.
  function logout() {
    const tokenSnapshot = token.value
    _clearLocalOnly()
    if (tokenSnapshot) {
      apiLogout(tokenSnapshot).catch(() => {
        // Failure is silent by design: local state is already cleared and
        // the server TTL is the fallback.
      })
    }
  }

  async function login(user: string, pass: string): Promise<string | null> {
    try {
      const resp = await apiLogin(user, pass)
      if (resp.status.code !== 0) {
        return resp.status.message || 'Login failed'
      }
      setAuth(resp)
      return null // null = success
    } catch (err: any) {
      return err.message || 'Network error'
    }
  }

  async function register(user: string, pass: string, displayName = ''): Promise<string | null> {
    try {
      const resp = await apiRegister(user, pass, displayName)
      if (resp.status.code !== 0) {
        return resp.status.message || 'Registration failed'
      }
      return null // null = success, user still needs to login
    } catch (err: any) {
      return err.message || 'Network error'
    }
  }

  // Wire token getter so all gRPC calls include auth header
  setAuthTokenGetter(() => token.value)

  // Wire unauthorized callback so 401 responses trigger a LOCAL logout
  // (passive): the server already refused the token, so there is nothing to
  // revoke. Only fire when the user actually had a session (prevents 401
  // during login triggering logout).
  setOnUnauthorized(() => {
    if (token.value) _clearLocalOnly()
  })

  // Periodic token expiry check — proactively logout when token expires
  // (Date.now() in computed isn't time-reactive, so we poll)
  _expiryCheckFn = () => {
    if (token.value && Date.now() > expiresAt.value) {
      _clearLocalOnly()
    }
  }
  if (!_expiryTimer) {
    _expiryTimer = setInterval(() => _expiryCheckFn?.(), 30_000)
  }

  return {
    userId,
    username,
    token,
    role,
    isAuthenticated,
    isAdmin,
    login,
    register,
    logout,
    _clearLocalOnly,
  }
})
