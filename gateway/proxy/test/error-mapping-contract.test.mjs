/**
 * Proxy gRPC error semantics — stable JSON/SSE error mapping,
 * Authorization metadata propagation and browser abort → stream.cancel().
 *
 * Runtime coverage: a real in-process gRPC server (grpc-js, loaded from the
 * repository proto sources) backs the real proxy (server.mjs). No mocking of
 * the proxy itself; every assertion goes through the HTTP surface.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import test from 'node:test';
import grpc from '@grpc/grpc-js';
import protoLoader from '@grpc/proto-loader';

const root = path.resolve(import.meta.dirname, '../../..');
const PROTO_DIR = path.join(root, 'proto');

const loaderOptions = {
  keepCase: true,
  longs: String,
  enums: String,
  defaults: true,
  oneofs: true,
  includeDirs: [PROTO_DIR],
};

async function freePort() {
  // Hardening: listen(0) → close leaves a port-reclaim window in
  // which another process can grab the same port, making the suite flaky.
  // Probe the returned port by re-binding it immediately (SO_REUSEADDR-style
  // check) and retry with a fresh port when the probe fails.
  const takePort = () =>
    new Promise((resolve, reject) => {
      const srv = net.createServer();
      srv.listen(0, '127.0.0.1', () => {
        const port = srv.address().port;
        srv.close(() => resolve(port));
      });
      srv.on('error', reject);
    });
  const reusable = (port) =>
    new Promise((resolve) => {
      const probe = net.createServer();
      probe.once('error', () => resolve(false));
      probe.listen(port, '127.0.0.1', () => {
        probe.close(() => resolve(true));
      });
    });
  for (let attempt = 0; attempt < 5; attempt++) {
    const port = await takePort();
    if (await reusable(port)) {
      return port;
    }
  }
  throw new Error('no reusable free port found');
}

// gRPC status codes under test
const CASES = [
  { username: 'err-unauth', code: grpc.status.UNAUTHENTICATED, name: 'UNAUTHENTICATED', http: 401 },
  { username: 'err-perm', code: grpc.status.PERMISSION_DENIED, name: 'PERMISSION_DENIED', http: 403 },
  { username: 'err-notfound', code: grpc.status.NOT_FOUND, name: 'NOT_FOUND', http: 404 },
  // ALREADY_EXISTS → 409 runtime contract (already in the mapping
  // table; this case pins it through the real HTTP surface).
  { username: 'err-exists', code: grpc.status.ALREADY_EXISTS, name: 'ALREADY_EXISTS', http: 409 },
  { username: 'err-exhausted', code: grpc.status.RESOURCE_EXHAUSTED, name: 'RESOURCE_EXHAUSTED', http: 429 },
  { username: 'err-cancelled', code: grpc.status.CANCELLED, name: 'CANCELLED', http: 499 },
  // Batch-8 R9: the backend returns these frequently (ExecutePlan argument
  // validation, circuit-open UNAVAILABLE, deadline contraction, unimplemented
  // RPCs) — they must not collapse into a generic 500.
  { username: 'err-invalid', code: grpc.status.INVALID_ARGUMENT, name: 'INVALID_ARGUMENT', http: 400 },
  { username: 'err-precondition', code: grpc.status.FAILED_PRECONDITION, name: 'FAILED_PRECONDITION', http: 412 },
  { username: 'err-unimplemented', code: grpc.status.UNIMPLEMENTED, name: 'UNIMPLEMENTED', http: 501 },
  { username: 'err-unavailable', code: grpc.status.UNAVAILABLE, name: 'UNAVAILABLE', http: 503 },
  { username: 'err-deadline', code: grpc.status.DEADLINE_EXCEEDED, name: 'DEADLINE_EXCEEDED', http: 504 },
];

// Mock gRPC backend (real grpc-js server over the repo protos)
const userDef = protoLoader.loadSync(path.join(PROTO_DIR, 'user.proto'), loaderOptions);
const userProto = grpc.loadPackageDefinition(userDef);
const agentDef = protoLoader.loadSync(path.join(PROTO_DIR, 'agent_service.proto'), loaderOptions);
const agentProto = grpc.loadPackageDefinition(agentDef);

const streamState = { cancelled: false, sawErrorRequest: false };

function loginHandler(call, callback) {
  const username = call.request.username;

  // Echo the inbound Authorization metadata back in the response so the test
  // can prove the proxy forwards the header end-to-end.
  if (username === 'echo-auth') {
    const auth = call.metadata.get('authorization');
    return callback(null, {
      status: { code: 0, message: 'ok', details: '' },
      user_id: 'u-echo',
      username: auth.length ? String(auth[0]) : '',
      token: 't',
      expires_at: 0,
      role: 'USER',
    });
  }

  const hit = CASES.find((c) => c.username === username);
  if (hit) {
    return callback({
      code: hit.code,
      details: `synthetic ${hit.name} from mock backend`,
    });
  }
  callback(null, {
    status: { code: 0, message: 'ok', details: '' },
    user_id: 'u',
    username,
    token: 't',
    expires_at: 0,
    role: 'USER',
  });
}

function watchHandler(call) {
  const service = call.request.service;
  if (service === 'err-perm') {
    streamState.sawErrorRequest = true;
    call.emit('error', {
      code: grpc.status.PERMISSION_DENIED,
      details: 'synthetic stream PERMISSION_DENIED from mock backend',
    });
    return;
  }
  // "hang" stream: emit one event then stay open until the client aborts.
  call.write({ status: 'SERVING' });
  const keepAlive = setInterval(() => {
    try {
      call.write({ status: 'SERVING' });
    } catch {
      clearInterval(keepAlive);
    }
  }, 25);
  call.on('cancelled', () => {
    streamState.cancelled = true;
    clearInterval(keepAlive);
  });
}

// Boot mock backend + real proxy
const grpcServer = new grpc.Server();
const logoutState = { sawAuth: '' };
grpcServer.addService(userProto.agent_communication.auth.UserService.service, {
  register: (call, cb) => cb(null, {
    status: { code: 0, message: 'ok', details: '' },
    user_id: 'u', username: call.request.username, role: 'USER',
  }),
  login: loginHandler,
  validateToken: (call, cb) => cb(null, {
    status: { code: 0, message: 'ok', details: '' },
    user_id: 'u', username: 'u', valid: true, role: 'USER',
  }),
  // P22: Logout revocation — the token must ride the metadata untouched
  // (the proxy never consumes or rewrites it for protected RPCs).
  logout: (call, cb) => {
    logoutState.sawAuth = (call.metadata.get('authorization') || [''])[0];
    cb(null, { status: { code: 0, message: 'ok', details: '' } });
  },
});
grpcServer.addService(agentProto.agent_communication.HealthService.service, {
  check: (call, cb) => cb(null, { status: 'SERVING' }),
  watch: watchHandler,
});
// Hardening: retry the bind with a fresh port instead of failing
// the whole suite on the rare reclaim race.
let grpcPort;
for (let attempt = 0; attempt < 5; attempt++) {
  grpcPort = await freePort();
  const bound = await new Promise((resolve) => {
    grpcServer.bindAsync(`127.0.0.1:${grpcPort}`, grpc.ServerCredentials.createInsecure(),
      (err) => resolve(!err));
  });
  if (bound) break;
  if (attempt === 4) throw new Error('grpc mock server could not bind a port');
}
const proxyPort = await freePort();

process.env.GRPC_TARGET = `127.0.0.1:${grpcPort}`;
process.env.PROXY_PORT = String(proxyPort);
const { default: proxyServer } = await import('../server.mjs');

const BASE = `http://127.0.0.1:${proxyPort}`;

async function unary(pathname, body, headers = {}) {
  const resp = await fetch(`${BASE}${pathname}`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', ...headers },
    body: JSON.stringify(body),
  });
  let parsed = null;
  try {
    parsed = await resp.json();
  } catch {
    // non-JSON body
  }
  return { status: resp.status, body: parsed };
}

// 1. Unary: the five required gRPC codes map to stable JSON + HTTP

for (const c of CASES) {
  test(`unary maps gRPC ${c.name} to HTTP ${c.http} with {error, code, details}`, async () => {
    const { status, body } = await unary('/agent_communication.auth.UserService/Login', {
      username: c.username,
      password: 'x',
    });

    assert.equal(status, c.http, `expected HTTP ${c.http} for ${c.name}`);
    assert.ok(body, 'error body must be JSON');
    assert.equal(body.code, c.code, 'numeric gRPC code must survive');
    assert.equal(body.code_name, c.name, 'stable code_name must survive');
    assert.match(String(body.error), new RegExp(c.name), 'error message keeps semantics');
    assert.match(String(body.details), /synthetic/, 'details must be propagated');
    // Never wrapped as a 200 success JSON.
    assert.notEqual(status, 200);
    assert.equal(body.status, undefined, 'must not masquerade as an app-level success');
  });
}

// 2. Authorization header reaches gRPC metadata untouched

test('Authorization header is forwarded to gRPC metadata', async () => {
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Login',
    { username: 'echo-auth', password: 'x' },
    { authorization: 'Bearer pr-f-token-123' },
  );

  assert.equal(status, 200);
  assert.equal(body.username, 'Bearer pr-f-token-123');
});

test('Logout forwards with the bearer token in gRPC metadata (P22)', async () => {
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    { authorization: 'Bearer pr-f-logout-token' },
  );

  assert.equal(status, 200);
  assert.equal(body.status?.code, 0);
  assert.equal(logoutState.sawAuth, 'Bearer pr-f-logout-token');
});

// 3. SSE: stream errors become structured error events, then close

test('stream errors are relayed as structured SSE error events with code semantics', async () => {
  const resp = await fetch(`${BASE}/agent_communication.HealthService/Watch`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ service: 'err-perm' }),
  });
  assert.equal(resp.status, 200, 'SSE headers are already committed; error rides in-band');
  assert.match(resp.headers.get('content-type') || '', /text\/event-stream/);

  const text = await resp.text();
  const events = text
    .split('\n\n')
    .map((f) => f.trim())
    .filter((f) => f.startsWith('data: '))
    .map((f) => JSON.parse(f.slice(6)));

  const errorEvents = events.filter((e) => e.event_type === 'error');
  assert.equal(errorEvents.length, 1, 'exactly one structured error event');
  assert.equal(errorEvents[0].code, grpc.status.PERMISSION_DENIED);
  assert.equal(errorEvents[0].code_name, 'PERMISSION_DENIED');
  assert.match(String(errorEvents[0].details ?? errorEvents[0].content), /synthetic/);
  // The stream must be closed after the error event (resp.text() resolving
  // proves the connection ended).
  assert.ok(streamState.sawErrorRequest);
});

// 4. Browser abort propagates to gRPC stream.cancel()

test('client abort cancels the upstream gRPC stream', async () => {
  streamState.cancelled = false;
  const controller = new AbortController();

  const resp = await fetch(`${BASE}/agent_communication.HealthService/Watch`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ service: 'hang' }),
    signal: controller.signal,
  });
  const reader = resp.body.getReader();
  const first = await reader.read();
  assert.ok(!first.done, 'stream must produce events before abort');
  controller.abort();
  await reader.read().catch(() => {});

  const deadline = Date.now() + 3000;
  while (!streamState.cancelled && Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 25));
  }
  assert.ok(streamState.cancelled, 'mock gRPC server must observe the cancellation');
});

// 5. Static guards on the mapping table

test('proxy source pins the required status mappings', () => {
  const server = fs.readFileSync(path.join(root, 'gateway/proxy/server.mjs'), 'utf8');

  // The guards pin the GRPC_HTTP_STATUS mapping CODE, not a
  // free-text comment that can drift from the table.
  assert.match(server, /\[grpc\.status\.UNAUTHENTICATED\]:\s*401/);
  assert.match(server, /\[grpc\.status\.PERMISSION_DENIED\]:\s*403/);
  assert.match(server, /\[grpc\.status\.NOT_FOUND\]:\s*404/);
  assert.match(server, /\[grpc\.status\.ALREADY_EXISTS\]:\s*409/);
  assert.match(server, /\[grpc\.status\.RESOURCE_EXHAUSTED\]:\s*429/);
  assert.match(server, /\[grpc\.status\.CANCELLED\]:\s*499/);
  // R9: no silent collapse into a generic 500 for these codes.
  assert.match(server, /\[grpc\.status\.INVALID_ARGUMENT\]:\s*400/);
  assert.match(server, /\[grpc\.status\.FAILED_PRECONDITION\]:\s*412/);
  assert.match(server, /\[grpc\.status\.UNIMPLEMENTED\]:\s*501/);
  assert.match(server, /\[grpc\.status\.UNAVAILABLE\]:\s*503/);
  assert.match(server, /\[grpc\.status\.DEADLINE_EXCEEDED\]:\s*504/);
  // Abort propagation must exist for the single streaming code path.
  assert.match(server, /stream\.cancel\(\)/);
  // R10: a trailing gRPC error after an in-band terminal event must not
  // inject a second error frame.
  assert.match(server, /if \(completeSeen\)/);
});

test('teardown', async () => {
  await new Promise((resolve) => proxyServer.close(resolve));
  await new Promise((resolve) => grpcServer.tryShutdown(resolve));
});
