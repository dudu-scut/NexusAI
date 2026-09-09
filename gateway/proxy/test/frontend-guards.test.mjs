/**
 * Static guards for the frontend surface.
 *
 * Node-side contract tests that read the frontend sources and assert the
 * cleanup/role-gating invariants cannot silently regress:
 *  - AdminView keeps no Cron/Canary leftovers
 *  - Dashboard/Monitor stay wired to the real PG durability RPCs
 *  - admin entry points are gated on the login-provided role
 *  - AgentSandbox exposes all four intervention decisions (MODIFY with text)
 *  - ChatView wires shareSession and the admin link is role-gated
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../../..');
const VIEWS = path.join(root, 'frontend/src/views');

function read(rel) {
  return fs.readFileSync(path.join(root, rel), 'utf8');
}

// 1. AdminView: no Cron/Canary leftovers

test('AdminView has no Cron/Canary state, tabs or handlers', () => {
  const admin = fs.readFileSync(path.join(VIEWS, 'AdminView.vue'), 'utf8');

  const forbidden = [
    'cronTasks',
    'canaryConfig',
    'addCronTask',
    'triggerCronTask',
    'deleteCronTask',
    'promoteCanary',
    'rollbackCanary',
    'ScheduledTask',
    'CanaryConfig',
    "id: 'cron'",
    "id: 'canary'",
    "activeTab === 'cron'",
    "activeTab === 'canary'",
  ];
  for (const token of forbidden) {
    assert.ok(
      !admin.includes(token),
      `AdminView must not contain Cron/Canary leftover "${token}"`,
    );
  }

  // The tabs that must survive the cleanup.
  for (const kept of ["id: 'dashboard'", "id: 'budget'", "id: 'replay'"]) {
    assert.ok(admin.includes(kept), `AdminView must keep tab ${kept}`);
  }
});

test('proto.ts no longer declares removed Cron/Canary interfaces', () => {
  const protoTs = read('frontend/src/types/proto.ts');
  assert.ok(!protoTs.includes('interface ScheduledTask'));
  assert.ok(!protoTs.includes('interface CanaryConfig'));
});

// 2. Dashboard/Monitor stay on real PG durability RPCs

test('Dashboard reads real observability/registry RPCs and shows honest states', () => {
  const dashboard = fs.readFileSync(path.join(VIEWS, 'Dashboard.vue'), 'utf8');
  for (const rpc of ['getAgents', 'getAgentMetrics', 'getCostReport']) {
    assert.ok(dashboard.includes(rpc), `Dashboard must call ${rpc}`);
  }
  // Four states: loading gate, data-unavailable warning, graceful fallback.
  assert.match(dashboard, /loading/, 'loading state required');
  assert.match(dashboard, /dataAvailable/, 'error/unavailable state required');
  assert.match(dashboard, /useFallbackData/, 'honest empty fallback required');
});

test('Monitor reads real registry/metrics RPCs and shows honest states', () => {
  const monitor = fs.readFileSync(path.join(VIEWS, 'Monitor.vue'), 'utf8');
  for (const rpc of ['getAgents', 'getAgentMetrics']) {
    assert.ok(monitor.includes(rpc), `Monitor must call ${rpc}`);
  }
  assert.match(monitor, /dataAvailable/, 'connection warning state required');
  assert.match(monitor, /EmptyState/, 'empty state required when no trace data');
  // deep-review D-F1: trace-based latency/error panels have no backend
  // enumeration RPC today (GetTraceDetail needs a trace id that nothing
  // lists), so Monitor must NOT fabricate a trace call — the honest
  // EmptyStates cover the missing channel until a recent-traces RPC exists.
  assert.ok(!monitor.includes('getTraceDetail'),
            'Monitor must not fake trace RPCs without a trace-list source');
});

// 3. Role gating (login role → admin entry points)

test('auth store persists the login role and exposes isAdmin', () => {
  const auth = read('frontend/src/stores/auth.ts');
  assert.match(auth, /const role = ref/, 'role ref required');
  assert.match(auth, /isAdmin = computed\(\(\) => role\.value === 'ADMIN'\)/);
  assert.match(auth, /role: role\.value/, 'role must be persisted to localStorage');
  assert.match(auth, /role\.value = data\.role \|\| 'USER'/, 'setAuth must adopt login role');
});

test('router guards /admin behind requiresAdmin', () => {
  const router = read('frontend/src/router/index.ts');
  assert.match(router, /requiresAdmin: true/, '/admin route must be flagged');
  assert.match(router, /to\.meta\.requiresAdmin && !auth\.isAdmin/, 'guard must check isAdmin');
});

test('ChatView hides the Admin entry for non-admin roles', () => {
  const chat = fs.readFileSync(path.join(VIEWS, 'ChatView.vue'), 'utf8');
  assert.match(chat, /v-if="authStore\.isAdmin" to="\/admin"/);
});

test('SideNav admin nav item is gated behind isAdmin', () => {
  const sidenav = read('frontend/src/components/layout/SideNav.vue');
  // navItems must be reactive on the role, not a static literal.
  assert.match(sidenav, /const navItems = computed\(/, 'navItems must be a computed');
  // The admin entry is only pushed when the auth store says ADMIN.
  assert.match(sidenav, /if \(auth\.isAdmin\)/);
  assert.match(
    sidenav,
    /if \(auth\.isAdmin\) \{\s*items\.push\(\{ label: '管理后台'[\s\S]*?path: '\/admin'/,
    'admin nav item must be added inside the isAdmin gate',
  );
  // No unconditional '/admin' nav literal may remain outside the gate.
  assert.ok(
    !/\{ label: '管理后台'[\s\S]*?\},?\s*\]/.test(sidenav),
    'admin item must not sit in an unconditional array literal',
  );
});

// 4. AgentSandbox: all four intervention decisions

test('AgentSandbox exposes PROCEED/MODIFY/SKIP/ABORT decisions', () => {
  const sandbox = fs.readFileSync(path.join(VIEWS, 'AgentSandbox.vue'), 'utf8');
  assert.match(sandbox, /resolveIntervention\('PROCEED'\)/);
  assert.match(sandbox, /resolveIntervention\('MODIFY'\)/);
  assert.match(sandbox, /resolveIntervention\('SKIP'\)/);
  assert.match(sandbox, /resolveIntervention\('ABORT'\)/);
  // MODIFY requires replacement text before it can be submitted.
  assert.match(sandbox, /modificationText/);
  assert.match(sandbox, /decision === 'MODIFY' && !modificationText\.value\.trim\(\)/);
});

// 5. ChatView: share entry point wired to shareSession

test('ChatView provides a share entry calling shareSession', () => {
  const chat = fs.readFileSync(path.join(VIEWS, 'ChatView.vue'), 'utf8');
  assert.match(chat, /import \{ shareSession \} from '\.\.\/services\/grpc-client'/);
  assert.match(chat, /shareSession\(chatStore\.contextId\)/);
  // Real error surfacing, never a fake success.
  assert.match(chat, /resp\.status\.code !== 0/);
});

test('chat store keeps failure reason visible and offers retry', () => {
  const store = read('frontend/src/stores/chat.ts');
  assert.match(store, /function retryLast/);
  assert.match(store, /case 'error':/);
  const chat = fs.readFileSync(path.join(VIEWS, 'ChatView.vue'), 'utf8');
  assert.match(chat, /chatStore\.retryLast\(\)/, 'retry entry must exist in ChatView');
});
