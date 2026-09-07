/**
 * NexusAI gRPC ↔ JSON Transcoding Proxy
 *
 * Serves the supported JSON browser gateway for local development and containers.
 * Accepts JSON POST from the Vue frontend, converts to gRPC/protobuf,
 * forwards to the rpc_server, and returns JSON.
 *
 * Listens on :8081 (same port Vite proxy expects).
 */

import http from 'node:http';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import grpc from '@grpc/grpc-js';
import protoLoader from '@grpc/proto-loader';
import { createClient } from 'redis';

// Config
const PROXY_PORT = parseInt(process.env.PROXY_PORT || '8081', 10);
const GRPC_TARGET = process.env.GRPC_TARGET || 'localhost:50051';
const PROTO_DIR = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  '../../proto'
);

// P26 T3 (trusted proxy): when NEXUSAI_TRUST_PROXY=1 the proxy performs the
// session check locally against Redis (deny/epoch/session keys, the same
// semantics as the backend AuthCache) and injects HMAC-signed identity
// headers instead of forwarding the raw Bearer. The shared secret is
// mandatory in trust mode — missing/too short refuses startup (fail-fast).
const REDIS_HOST = process.env.REDIS_HOST || '127.0.0.1';
const REDIS_PORT = parseInt(process.env.REDIS_PORT || '6379', 10);
const TRUST_PROXY = process.env.NEXUSAI_TRUST_PROXY === '1';
const PROXY_HMAC_SECRET = process.env.NEXUSAI_PROXY_HMAC_SECRET || '';
if (TRUST_PROXY && PROXY_HMAC_SECRET.length < 32) {
  console.error(
    '[proxy] NEXUSAI_TRUST_PROXY=1 requires NEXUSAI_PROXY_HMAC_SECRET ' +
    '(>= 32 chars); refusing to start in trust mode',
  );
  process.exit(1);
}

// Load Proto Definitions
const loaderOptions = {
  keepCase: true,
  longs: String,
  enums: String,
  defaults: true,
  oneofs: true,
  includeDirs: [PROTO_DIR],
};

const pkgs = {};

function loadProto(file) {
  const def = protoLoader.loadSync(path.join(PROTO_DIR, file), loaderOptions);
  return grpc.loadPackageDefinition(def);
}

// Load all proto packages
const agentProto = loadProto('agent_service.proto');
const queryProto = loadProto('ai_query.proto');
const observabilityProto = loadProto('observability.proto');
const userProto = loadProto('user.proto');
const orchestrationProto = loadProto('orchestration.proto');
const sharingProto = loadProto('sharing.proto');
const lifecycleProto = loadProto('agent_lifecycle.proto');
const experienceProto = loadProto('user_experience.proto');

// Create gRPC Clients
const clients = {};

function initClients() {
  const creds = grpc.credentials.createInsecure();

  clients['agent_communication.AgentCommunicationService'] =
    new agentProto.agent_communication.AgentCommunicationService(GRPC_TARGET, creds);

  clients['agent_communication.AIQueryService'] =
    new queryProto.agent_communication.AIQueryService(GRPC_TARGET, creds);

  clients['agent_communication.ObservabilityService'] =
    new observabilityProto.agent_communication.ObservabilityService(GRPC_TARGET, creds);

  clients['agent_communication.auth.UserService'] =
    new userProto.agent_communication.auth.UserService(GRPC_TARGET, creds);

  clients['agent_communication.HealthService'] =
    new agentProto.agent_communication.HealthService(GRPC_TARGET, creds);

  clients['agent_communication.OrchestrationService'] =
    new orchestrationProto.agent_communication.OrchestrationService(GRPC_TARGET, creds);

  clients['agent_communication.SharingService'] =
    new sharingProto.agent_communication.SharingService(GRPC_TARGET, creds);

  clients['agent_communication.AgentLifecycleService'] =
    new lifecycleProto.agent_communication.AgentLifecycleService(GRPC_TARGET, creds);

  clients['agent_communication.UserExperienceService'] =
    new experienceProto.agent_communication.UserExperienceService(GRPC_TARGET, creds);

  console.log(`gRPC clients initialized → ${GRPC_TARGET}`);
}

// Streaming RPC Classification
const SERVER_STREAMING_RPCS = new Set([
  'agent_communication.AIQueryService/QueryStream',
  'agent_communication.AgentCommunicationService/ListenMessages',
  'agent_communication.HealthService/Watch',
  'agent_communication.SharingService/ObserveSession',
]);

const UNSUPPORTED_STREAMING_RPCS = new Set([
  'agent_communication.AgentCommunicationService/BatchSendMessages',
  'agent_communication.AgentCommunicationService/RealTimeCommunication',
]);

// gRPC status → HTTP status mapping
// Stable error semantics: gRPC errors are surfaced as structured JSON/SSE
// errors, never wrapped as 200 success responses.
const GRPC_HTTP_STATUS = {
  [grpc.status.CANCELLED]: 499,
  [grpc.status.PERMISSION_DENIED]: 403,
  [grpc.status.NOT_FOUND]: 404,
  [grpc.status.ALREADY_EXISTS]: 409,
  [grpc.status.RESOURCE_EXHAUSTED]: 429,
  [grpc.status.UNAUTHENTICATED]: 401,
  // Frequenty returned by the backend: argument validation, preconditions,
  // availability/deadline, unimplemented RPCs. Mapping them to 500 used to
  // hide "the caller sent garbage" / "backend overloaded" behind a generic
  // server-error code and broke the frontend's differentiated handling.
  [grpc.status.INVALID_ARGUMENT]: 400,
  [grpc.status.OUT_OF_RANGE]: 400,
  [grpc.status.FAILED_PRECONDITION]: 412,
  [grpc.status.UNIMPLEMENTED]: 501,
  [grpc.status.UNAVAILABLE]: 503,
  [grpc.status.DATA_LOSS]: 500,
  [grpc.status.INTERNAL]: 500,
  [grpc.status.UNKNOWN]: 500,
  [grpc.status.DEADLINE_EXCEEDED]: 504,
};
// NOTE: the mapping table above is the single source of truth — the
// error-mapping contract test's static guard asserts on these computed
// entries, not on any prose comment.

const GRPC_STATUS_NAME = Object.fromEntries(
  Object.entries(grpc.status)
    .filter(([k]) => Number.isNaN(Number(k)))
    .map(([name, code]) => [code, name])
);

function grpcErrorPayload(err, fallbackMessage) {
  const code = typeof err?.code === 'number' ? err.code : null;
  const codeName = code != null ? (GRPC_STATUS_NAME[code] ?? 'UNKNOWN') : 'UNKNOWN';
  const details = err?.details || err?.message || fallbackMessage;
  return {
    error: `${codeName}: ${details}`,
    code: code ?? 'N/A',
    code_name: codeName,
    details,
  };
}

// Helper: Extract auth metadata
function buildMetadata(headers) {
  const meta = new grpc.Metadata();
  const auth = headers['authorization'];
  if (auth) {
    meta.add('authorization', auth);
  }
  return meta;
}

// ============================================================================
// P26 T3 trusted-proxy local authentication (only active when
// NEXUSAI_TRUST_PROXY=1; default-off keeps the legacy passthrough exactly)
// ============================================================================

// RPCs the proxy forwards WITHOUT local authentication (must mirror the
// backend whitelist): auth bootstrap + the restricted public share read +
// liveness probes (never carry a Bearer).
const AUTH_BYPASS_RPCS = new Set([
  'agent_communication.auth.UserService/Register',
  'agent_communication.auth.UserService/Login',
  'agent_communication.auth.UserService/ValidateToken',
  'agent_communication.SharingService/ReadSharedConversation',
  'agent_communication.HealthService/Check',
  'agent_communication.HealthService/Watch',
]);

function sha256Hex(input) {
  return crypto.createHash('sha256').update(input).digest('hex');
}

function hmacSha256Hex(canonical) {
  return crypto
    .createHmac('sha256', PROXY_HMAC_SECRET)
    .update(canonical)
    .digest('hex');
}

// The backend Bearer grammar: 64–256 lowercase/uppercase hex after "Bearer ".
function bearerTokenFrom(headers) {
  const authorization = headers['authorization'];
  if (typeof authorization !== 'string') return '';
  const match = /^Bearer\s+([0-9a-fA-F]{64,256})$/.exec(authorization.trim());
  return match ? match[1] : '';
}

// Redis access is abstracted behind a lazy singleton so contract tests can
// inject an in-memory stub (__setAuthCacheBackendForTest) without a real
// Redis; production uses the shared instance the backend also reads.
let lazyRedis = null;
let injectedBackend = null;

function authCacheBackend() {
  if (injectedBackend) return injectedBackend;
  if (!lazyRedis) {
    lazyRedis = createClient({
      url: `redis://${REDIS_HOST}:${REDIS_PORT}`,
      socket: { connectTimeout: 1000, reconnectStrategy: false },
    });
    lazyRedis.on('error', (err) => {
      // Backend degrades to the ValidateToken fallback on Redis errors.
      console.warn(`[proxy] auth-cache Redis error: ${err.message}`);
    });
  }
  return lazyRedis;
}

export function __setAuthCacheBackendForTest(backend) {
  injectedBackend = backend;
}

async function redisGet(key) {
  const client = authCacheBackend();
  if (!client) return null;
  try {
    if (typeof client.isReady === 'boolean' && !client.isReady) {
      await client.connect();
    }
    return await client.get(key);
  } catch {
    return null;  // Redis unavailable → caller falls back to ValidateToken
  }
}

// Miss-fallback: authoritative backend ValidateToken RPC (whitelisted, so no
// auth metadata is needed). The backend refills the session cache on success
// (AuthCache::validateTokenCached), so the next request hits Redis again.
function validateTokenViaRpc(token, tokenHash) {
  return new Promise((resolve) => {
    const client = clients['agent_communication.auth.UserService'];
    if (!client) {
      return resolve({ ok: false, reason: 'UserService unavailable' });
    }
    client.validateToken({ token }, new grpc.Metadata(), (err, resp) => {
      if (err || !resp || !resp.valid || !resp.user_id) {
        return resolve({ ok: false, reason: 'Token invalid or expired' });
      }
      resolve({
        ok: true,
        identity: {
          user_id: resp.user_id,
          username: resp.username || '',
          role: resp.role || 'USER',
          token_hash: tokenHash,
        },
      });
    });
  });
}

// Local auth check with the SAME fail-safe semantics as the backend cache:
// deny hit → refuse; session hit with matching epoch and unexpired payload →
// accept with zero backend calls; anything else → ValidateToken fallback.
// Only false negatives (a valid user refused once) are allowed.
async function authenticateRequest(headers) {
  const token = bearerTokenFrom(headers);
  if (!token) {
    return { ok: false, reason: 'Missing bearer token' };
  }
  const tokenHash = sha256Hex(token);

  const deny = await redisGet(`auth:deny:${tokenHash}`);
  if (deny) {
    return { ok: false, reason: 'Session revoked' };
  }

  const session = await redisGet(`auth:session:${tokenHash}`);
  if (session) {
    try {
      const payload = JSON.parse(session);
      const now = Math.floor(Date.now() / 1000);
      let epoch = 0;
      if (payload.user_id) {
        const raw = await redisGet(`auth:epoch:${payload.user_id}`);
        const parsed = raw != null ? Number(raw) : 0;
        epoch = Number.isFinite(parsed) ? parsed : 0;
      }
      const notExpired = Number(payload.expires_at) > now;
      if (
        payload.user_id && payload.username && payload.role &&
        Number(payload.epoch) === epoch && notExpired
      ) {
        return {
          ok: true,
          identity: {
            user_id: payload.user_id,
            username: payload.username,
            role: payload.role,
            token_hash: tokenHash,
          },
        };
      }
      // Epoch mismatch / stale payload → authoritative re-validation below.
    } catch {
      // Corrupt payload → authoritative re-validation below.
    }
  }

  return validateTokenViaRpc(token, tokenHash);
}

// Strip any client-supplied x-nexusai-* headers (never forwarded) and inject
// the five identity headers + HMAC. canonical excludes the HTTP method by
// design: Bearer leakage already allows cross-method impersonation and the
// 60s exp is tighter than the status quo.
function trustedMetadata(identity) {
  const meta = new grpc.Metadata();
  const exp = Math.floor(Date.now() / 1000) + 60;
  const canonical = [
    identity.user_id,
    identity.username,
    identity.role,
    String(exp),
    identity.token_hash,
  ].join('|');
  meta.add('x-nexusai-user-id', identity.user_id);
  meta.add('x-nexusai-username', identity.username);
  meta.add('x-nexusai-role', identity.role);
  meta.add('x-nexusai-exp', String(exp));
  meta.add('x-nexusai-token-hash', identity.token_hash);
  meta.add('x-nexusai-signature', hmacSha256Hex(canonical));
  return meta;
}

// Helper: derive a gRPC deadline from the request body's timeout_seconds
// (AIQueryRequest.timeout_seconds, sent by the frontend). Absent/invalid
// field → undefined → grpc-js leaves the deadline unset (byte-equivalent
// legacy behavior). P20: this activates the server-side deadline
// contraction path.
function deadlineFromBody(body) {
  const t = body && body.timeout_seconds;
  return typeof t === 'number' && Number.isFinite(t) && t > 0
    ? new Date(Date.now() + t * 1000)
    : undefined;
}

// Helper: Convert Buffer fields to base64
// protobuf bytes fields come back as Buffer objects from grpc-js.
// JSON.stringify renders them as {"type":"Buffer","data":[...]}.
// This function recursively walks the object and converts Buffers to base64.
function sanitizeBuffers(obj) {
  if (obj == null || typeof obj !== 'object') return obj;
  if (Buffer.isBuffer(obj)) return obj.toString('base64');
  if (Array.isArray(obj)) return obj.map(sanitizeBuffers);
  const result = {};
  for (const key of Object.keys(obj)) {
    result[key] = sanitizeBuffers(obj[key]);
  }
  return result;
}

// Unary RPC Handler
function unaryCall(serviceName, methodName, body, metadata) {
  return new Promise((resolve, reject) => {
    const client = clients[serviceName];
    if (!client) {
      return reject(new Error(`Unknown service: ${serviceName}`));
    }

    // grpc-js method names are camelCase
    const grpcMethod = methodName[0].toLowerCase() + methodName.slice(1);
    if (typeof client[grpcMethod] !== 'function') {
      return reject(new Error(`Unknown method: ${serviceName}.${methodName}`));
    }

    const deadline = deadlineFromBody(body);
    const options = deadline !== undefined ? { deadline } : {};
    client[grpcMethod](body, metadata, options, (err, response) => {
      if (err) {
        reject(err);
      } else {
        resolve(response);
      }
    });
  });
}

// Server-Streaming RPC Handler
function streamCall(serviceName, methodName, body, metadata, res) {
  const client = clients[serviceName];
  if (!client) {
    res.writeHead(500);
    res.end(`Unknown service: ${serviceName}`);
    return;
  }

  const grpcMethod = methodName[0].toLowerCase() + methodName.slice(1);
  if (typeof client[grpcMethod] !== 'function') {
    res.writeHead(500);
    res.end(`Unknown method: ${serviceName}.${methodName}`);
    return;
  }

  res.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'Connection': 'keep-alive',
    'Access-Control-Allow-Origin': '*',
  });

  const deadline = deadlineFromBody(body);
  const options = deadline !== undefined ? { deadline } : {};
  const stream = client[grpcMethod](body, metadata, options);
  let ended = false;
  // The gRPC server is the single authoritative emitter of terminal
  // events. Track whether a terminal event (complete/error) was relayed and
  // only synthesize a fallback complete when gRPC ended without one.
  let completeSeen = false;

  stream.on('data', (event) => {
    if (ended) return;
    // B1/P24 contract: a plan-only run ends with status awaiting_confirmation
    // and no server terminal event — treat it as terminal so the fallback
    // complete frame never clobbers the frontend's confirm/abandon flow.
    if (
      event &&
      (event.event_type === 'complete' ||
        event.event_type === 'error' ||
        (event.event_type === 'status' && event.content === 'awaiting_confirmation'))
    ) {
      completeSeen = true;
    }
    const json = JSON.stringify(sanitizeBuffers(event));
    res.write(`data: ${json}\n\n`);
  });

  stream.on('end', () => {
    if (ended) return;
    ended = true;
    if (!completeSeen) {
      res.write(`data: ${JSON.stringify({ event_type: 'complete' })}\n\n`);
    }
    res.end();
  });

  stream.on('error', (err) => {
    if (ended) return;
    // If an in-band terminal event (complete/error) was already relayed, the
    // client has seen the authoritative terminal — a trailing gRPC status
    // error (e.g. INTERNAL after the server emitted its 'error' event) must
    // not inject a SECOND error frame, which used to overwrite the sanitized
    // business error on the frontend and duplicate the activity entry.
    if (completeSeen) {
      ended = true;
      res.end();
      return;
    }
    ended = true;
    const codeLabel = err.code != null ? err.code : 'N/A';
    console.error(`[proxy] Stream error (${serviceName}.${methodName}):`, err.message, `(code: ${codeLabel})`);
    // Structured SSE error event with stable code semantics, then close.
    const payload = grpcErrorPayload(err, 'Stream error');
    const errJson = JSON.stringify({
      event_type: 'error',
      content: payload.error,
      code: payload.code,
      code_name: payload.code_name,
      details: payload.details,
    });
    res.write(`data: ${errJson}\n\n`);
    res.end();
  });

  // Handle client disconnect: browser/SSE close must cancel the gRPC stream
  // so the server can persist the cancelled terminal state.
  res.on('close', () => {
    if (!ended) {
      ended = true;
      stream.cancel();
    }
  });
}

// HTTP Request Handler
function handleRequest(req, res) {
  // CORS preflight
  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Access-Control-Allow-Headers': 'content-type, authorization, x-api-key',
      'Access-Control-Max-Age': '86400',
    });
    return res.end();
  }

  // Only accept POST
  if (req.method !== 'POST') {
    res.writeHead(405);
    return res.end('Method Not Allowed');
  }

  // Parse URL: /<service>/<method>
  const urlPath = req.url.split('?')[0]; // strip query string
  const parts = urlPath.split('/').filter(Boolean);
  if (parts.length !== 2) {
    res.writeHead(400, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify({ error: 'Invalid path. Expected /<service>/<method>' }));
  }

  const [serviceName, methodName] = parts;

  // Check service exists
  if (!clients[serviceName]) {
    res.writeHead(404, {
      'Content-Type': 'application/json',
      'Access-Control-Allow-Origin': '*',
    });
    return res.end(JSON.stringify({ error: `Unknown service: ${serviceName}` }));
  }

  // Read JSON body (with 1MB size limit to prevent DoS)
  const MAX_BODY_SIZE = 1024 * 1024; // 1MB
  const contentLength = parseInt(req.headers['content-length'] || '0', 10);
  if (contentLength > MAX_BODY_SIZE) {
    res.writeHead(413, { 'Content-Type': 'application/json' });
    return res.end(JSON.stringify({ error: 'Request body too large (max 1MB)' }));
  }

  // Collect Buffers and decode once: string-concatenating chunks splits
  // multi-byte UTF-8 characters (e.g. Chinese text) at chunk boundaries.
  const bodyChunks = [];
  let bodySize = 0;
  let bodyRejected = false;
  req.on('data', (chunk) => {
    bodySize += chunk.length;
    if (bodySize > MAX_BODY_SIZE) {
      if (bodyRejected) return;
      bodyRejected = true;
      res.writeHead(413, { 'Content-Type': 'application/json' });
      // Destroy only after the response has flushed, or the client may see
      // a connection reset instead of the 413.
      res.end(JSON.stringify({ error: 'Request body too large (max 1MB)' }), () => {
        req.destroy();
      });
      return;
    }
    bodyChunks.push(chunk);
  });
  req.on('end', async () => {
    if (bodyRejected) return;
    let parsed;
    try {
      const body = Buffer.concat(bodyChunks).toString('utf8');
      parsed = body ? JSON.parse(body) : {};
    } catch (e) {
      res.writeHead(400, { 'Content-Type': 'application/json' });
      return res.end(JSON.stringify({ error: 'Invalid JSON' }));
    }

    // Classify RPC by streaming type
    const rpcPath = serviceName + '/' + methodName;

    // Client-streaming and bidirectional are unsupported over HTTP JSON
    if (UNSUPPORTED_STREAMING_RPCS.has(rpcPath)) {
      res.writeHead(501, {
        'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*',
      });
      return res.end(JSON.stringify({
        error: 'RPC streaming mode is not supported by the JSON proxy',
        rpc: rpcPath,
      }));
    }

    // P26 T3: in trust mode every non-bypass RPC is authenticated locally
    // BEFORE any backend call (unary or stream — a failing check answers a
    // plain 401, never a half-open SSE). The Bearer is consumed here and
    // replaced by HMAC-signed identity headers; client-supplied x-nexusai-*
    // headers are never forwarded (strip-before-inject). Default mode keeps
    // the legacy passthrough (backend owns authentication).
    let metadata;
    if (TRUST_PROXY && !AUTH_BYPASS_RPCS.has(rpcPath)) {
      const auth = await authenticateRequest(req.headers);
      if (!auth.ok) {
        const payload = {
          error: `UNAUTHENTICATED: ${auth.reason}`,
          code: grpc.status.UNAUTHENTICATED,
          code_name: 'UNAUTHENTICATED',
          details: auth.reason,
        };
        res.writeHead(401, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*',
        });
        return res.end(JSON.stringify(payload));
      }
      metadata = trustedMetadata(auth.identity);
    } else {
      metadata = buildMetadata(req.headers);
    }

    // Server-streaming → SSE
    if (SERVER_STREAMING_RPCS.has(rpcPath)) {
      return streamCall(serviceName, methodName, parsed, metadata, res);
    }

    // Unary call
    try {
      const response = await unaryCall(serviceName, methodName, parsed, metadata);
      res.writeHead(200, {
        'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*',
      });
      res.end(JSON.stringify(sanitizeBuffers(response)));
    } catch (err) {
      const codeLabel = err.code != null ? err.code : 'N/A';
      console.error(`[proxy] RPC error (${serviceName}.${methodName}):`, err.message, `(code: ${codeLabel})`);
      // Map the five contract codes (plus ALREADY_EXISTS) to stable
      // HTTP statuses; everything else is a generic 500.
      const status = (typeof err.code === 'number' && GRPC_HTTP_STATUS[err.code]) || 500;
      const payload = grpcErrorPayload(err, 'RPC failed');

      res.writeHead(status, {
        'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*',
      });
      res.end(JSON.stringify(payload));
    }
  });
}

// Start Server
initClients();

const server = http.createServer(handleRequest);
server.listen(PROXY_PORT, () => {
  console.log(`NexusAI gRPC-JSON proxy listening on :${PROXY_PORT}`);
  console.log(`Forwarding to gRPC server at ${GRPC_TARGET}`);
});

// Exported so contract tests can shut the listener down (keeps `npm test`
// from hanging on an open handle); production entrypoint is unchanged.
export default server;
