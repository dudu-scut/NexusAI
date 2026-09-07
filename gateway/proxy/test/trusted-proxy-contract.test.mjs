/**
 * P26 T3 trusted-proxy contract (batch 11 phase 3).
 *
 * Boots the real proxy with NEXUSAI_TRUST_PROXY=1 + a shared HMAC secret and
 * an in-memory auth-cache backend (no real Redis needed): exercises the local
 * deny/epoch/session checks, the ValidateToken fallback, the five injected
 * identity headers + signature, client header stripping, and the plain-401
 * (never half-open SSE) failure path. The mock backend records the metadata
 * it actually receives so header-level contracts are asserted end-to-end.
 *
 * The proxy module is imported AFTER the env vars are set (module-scope
 * constants read them at import time).
 */

process.env.NEXUSAI_TRUST_PROXY = '1';
process.env.NEXUSAI_PROXY_HMAC_SECRET = 'test-shared-secret-0123456789abcdef'; // >= 32 chars
process.env.GRPC_TARGET = '127.0.0.1:0'; // placeholder; reset after mock binds

import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import net from 'node:net';
import test from 'node:test';
import grpc from '@grpc/grpc-js';
import protoLoader from '@grpc/proto-loader';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(import.meta.dirname, '../../..');

const loaderOptions = {
  keepCase: true,
  longs: String,
  enums: String,
  defaults: true,
  oneofs: true,
  includeDirs: [path.join(root, 'proto')],
};

const userProto = grpc.loadPackageDefinition(
  protoLoader.loadSync(path.join(root, 'proto/user.proto'), loaderOptions),
);

const SECRET = process.env.NEXUSAI_PROXY_HMAC_SECRET;

function sha256Hex(input) {
  return crypto.createHash('sha256').update(input).digest('hex');
}

function hmacHex(canonical) {
  return crypto.createHmac('sha256', SECRET).update(canonical).digest('hex');
}

// 64-hex token (backend Bearer grammar) + its Redis key names.
const TOKEN = 'a'.repeat(64);
const TOKEN_HASH = sha256Hex(TOKEN);
const USER_ID = 'trusted-user-001';
const USERNAME = 'trusted-user';
const ROLE = 'USER';
const NOW = Math.floor(Date.now() / 1000);
const SESSION_PAYLOAD = JSON.stringify({
  user_id: USER_ID,
  username: USERNAME,
  role: ROLE,
  epoch: 0,
  expires_at: NOW + 3600,
});

// In-memory auth-cache backend (deny / session / epoch keys).
function memoryBackend(entries) {
  const map = new Map(Object.entries(entries));
  return {
    isReady: true,
    connect: async () => {},
    get: async (key) => (map.has(key) ? map.get(key) : null),
    set: async (key, value) => map.set(key, value),
  };
}

function freePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.listen(0, '127.0.0.1', () => {
      const port = srv.address().port;
      srv.close(() => resolve(port));
    });
    srv.on('error', reject);
  });
}

// --- boot mock backend + real proxy -----------------------------------------

let grpcPort;
const observed = { lastMetadata: null, lastBody: null, validateCalls: 0, validateResult: { valid: true } };

const grpcServer = new grpc.Server();
grpcServer.addService(userProto.agent_communication.auth.UserService.service, {
  register: (call, cb) => cb(null, {
    status: { code: 0, message: 'ok', details: '' },
    user_id: 'u', username: call.request.username, role: 'USER',
  }),
  login: (call, cb) => {
    observed.lastMetadata = call.metadata;
    observed.lastBody = call.request;
    cb(null, {
      status: { code: 0, message: 'ok', details: '' },
      user_id: 'u', username: call.request.username, token: TOKEN,
      expires_at: NOW + 3600, role: 'USER',
    });
  },
  validateToken: (call, cb) => {
    observed.validateCalls += 1;
    const valid = observed.validateResult.valid;
    cb(null, {
      status: { code: valid ? 0 : 401, message: valid ? 'ok' : 'invalid', details: '' },
      user_id: valid ? USER_ID : '',
      username: valid ? USERNAME : '',
      valid,
      role: valid ? ROLE : '',
    });
  },
  logout: (call, cb) => {
    observed.lastMetadata = call.metadata;
    observed.lastBody = call.request;
    cb(null, { status: { code: 0, message: 'ok', details: '' } });
  },
});

const bound = await (async () => {
  for (let attempt = 0; attempt < 5; attempt++) {
    grpcPort = await freePort();
    const ok = await new Promise((resolve) => {
      grpcServer.bindAsync(`127.0.0.1:${grpcPort}`, grpc.ServerCredentials.createInsecure(),
        (err) => resolve(!err));
    });
    if (ok) return true;
  }
  return false;
})();
if (!bound) throw new Error('mock grpc server could not bind');

process.env.GRPC_TARGET = `127.0.0.1:${grpcPort}`;
const proxyPort = await freePort();
process.env.PROXY_PORT = String(proxyPort);

const { default: proxyServer, __setAuthCacheBackendForTest } = await import('../server.mjs');

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

function assertTrustedHeaders(meta) {
  const get = (k) => meta.get(k)[0];
  assert.equal(get('x-nexusai-user-id'), USER_ID);
  assert.equal(get('x-nexusai-username'), USERNAME);
  assert.equal(get('x-nexusai-role'), ROLE);
  assert.ok(!meta.get('authorization')?.length, 'Bearer must be consumed, never forwarded');
  const exp = Number(get('x-nexusai-exp'));
  assert.ok(Number.isFinite(exp) && exp > NOW && exp <= NOW + 60, 'exp must be a fresh 60s window');
  assert.equal(get('x-nexusai-token-hash'), TOKEN_HASH);
  const canonical = [USER_ID, USERNAME, ROLE, String(exp), TOKEN_HASH].join('|');
  assert.equal(get('x-nexusai-signature'), hmacHex(canonical), 'signature must cover all five fields');
}

// --- tests ------------------------------------------------------------------

test('trusted proxy: cache hit injects signed identity headers (zero backend auth calls)', async () => {
  __setAuthCacheBackendForTest(memoryBackend({
    [`auth:session:${TOKEN_HASH}`]: SESSION_PAYLOAD,
  }));
  observed.lastMetadata = null;
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    { authorization: `Bearer ${TOKEN}` },
  );

  assert.equal(status, 200);
  assert.equal(body.status?.code, 0);
  assert.ok(observed.lastMetadata, 'backend must have seen the call');
  assertTrustedHeaders(observed.lastMetadata);
  // Client-supplied x-nexusai-* headers must never reach the backend.
  const { status: stripStatus } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    {
      authorization: `Bearer ${TOKEN}`,
      'x-nexusai-user-id': 'forged-admin',
      'x-nexusai-role': 'ADMIN',
      'x-nexusai-signature': 'forged',
    },
  );
  assert.equal(stripStatus, 200);
  assertTrustedHeaders(observed.lastMetadata);
});

test('trusted proxy: deny hit answers 401 without touching the backend', async () => {
  __setAuthCacheBackendForTest(memoryBackend({
    [`auth:deny:${TOKEN_HASH}`]: '1',
  }));
  observed.validateCalls = 0;
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    { authorization: `Bearer ${TOKEN}` },
  );

  assert.equal(status, 401);
  assert.equal(body.code, grpc.status.UNAUTHENTICATED);
  assert.equal(body.code_name, 'UNAUTHENTICATED');
  assert.equal(observed.validateCalls, 0, 'deny hit must not fall back');
});

test('trusted proxy: epoch mismatch falls back and a refused token answers 401', async () => {
  const stalePayload = JSON.stringify({
    user_id: USER_ID, username: USERNAME, role: ROLE, epoch: 0, expires_at: NOW + 3600,
  });
  __setAuthCacheBackendForTest(memoryBackend({
    [`auth:session:${TOKEN_HASH}`]: stalePayload,
    [`auth:epoch:${USER_ID}`]: '3',  // INCR fired after the entry was written
  }));
  observed.validateResult = { valid: false };
  observed.validateCalls = 0;
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    { authorization: `Bearer ${TOKEN}` },
  );

  assert.equal(status, 401);
  assert.equal(observed.validateCalls, 1, 'stale entry must trigger the authoritative fallback');
  assert.equal(body.code_name, 'UNAUTHENTICATED');
});

test('trusted proxy: cache miss falls back to ValidateToken and injects on success', async () => {
  __setAuthCacheBackendForTest(memoryBackend({}));
  observed.validateResult = { valid: true };
  observed.validateCalls = 0;
  observed.lastMetadata = null;
  const { status } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    { authorization: `Bearer ${TOKEN}` },
  );

  assert.equal(status, 200);
  assert.equal(observed.validateCalls, 1, 'miss must consult the authoritative RPC once');
  assertTrustedHeaders(observed.lastMetadata);
});

test('trusted proxy: missing bearer answers 401 before any backend call', async () => {
  __setAuthCacheBackendForTest(memoryBackend({}));
  observed.validateCalls = 0;
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Logout',
    {},
    {},
  );

  assert.equal(status, 401);
  assert.equal(body.code_name, 'UNAUTHENTICATED');
  assert.equal(observed.validateCalls, 0);
});

test('trusted proxy: stream RPC with an invalid token gets a plain 401, not SSE', async () => {
  __setAuthCacheBackendForTest(memoryBackend({ [`auth:deny:${TOKEN_HASH}`]: '1' }));
  const resp = await fetch(`${BASE}/agent_communication.AIQueryService/QueryStream`, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      authorization: `Bearer ${TOKEN}`,
    },
    body: JSON.stringify({ question: 'x' }),
  });

  assert.equal(resp.status, 401, 'auth failure must not open an SSE stream');
  const contentType = resp.headers.get('content-type') || '';
  assert.ok(!contentType.includes('text/event-stream'));
  const body = await resp.json();
  assert.equal(body.code_name, 'UNAUTHENTICATED');
});

test('trusted proxy: whitelisted RPCs keep the legacy passthrough (Bearer forwarded)', async () => {
  __setAuthCacheBackendForTest(memoryBackend({}));
  observed.lastMetadata = null;
  const { status, body } = await unary(
    '/agent_communication.auth.UserService/Login',
    { username: 'echo', password: 'x' },
    { authorization: `Bearer ${TOKEN}` },
  );

  assert.equal(status, 200);
  assert.ok(body.token, 'login still returns a token');
  const forwarded = observed.lastMetadata?.get('authorization')?.[0];
  assert.equal(forwarded, `Bearer ${TOKEN}`, 'bypass RPCs must forward the Bearer untouched');
  assert.ok(!observed.lastMetadata?.get('x-nexusai-signature')?.length,
    'no identity injection on bypass RPCs');
});

test('teardown', async () => {
  await new Promise((resolve) => proxyServer.close(resolve));
  await new Promise((resolve) => grpcServer.tryShutdown(resolve));
});
