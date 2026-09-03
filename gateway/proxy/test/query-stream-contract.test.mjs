/**
 * Query/QueryStream durable pipeline — gateway & server contracts.
 *
 * Static contract assertions over the proxy, the C++ query service and the
 * multi-agent handler. Runtime coverage lives in
 * tests/test_durable_query_pipeline.cpp (real PostgreSQL + gRPC server).
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../../..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');

const countMatches = (text, regex) => (text.match(regex) || []).length;

// Node proxy: single terminal event per stream

test('proxy streamCall tracks completeSeen and relays terminal events', () => {
  const server = read('gateway/proxy/server.mjs');
  const start = server.indexOf('function streamCall(');
  assert.ok(start >= 0, 'missing streamCall');
  const streamCall = server.slice(start, server.indexOf('\n// HTTP Request Handler'));

  assert.match(streamCall, /let completeSeen = false;/);
  assert.match(streamCall, /event_type === 'complete' \|\| event\.event_type === 'error'/);
  assert.match(streamCall, /completeSeen = true;/);
});

test('proxy only synthesizes a fallback complete when gRPC sent no terminal', () => {
  const server = read('gateway/proxy/server.mjs');

  assert.match(server, /if \(!completeSeen\) \{/);
  // Exactly one synthesized complete payload in the whole file.
  assert.equal(
    countMatches(server, /event_type: 'complete'/g),
    1,
    'proxy must synthesize at most one fallback complete event',
  );
});

test('proxy cancels the gRPC stream when the browser/SSE connection closes', () => {
  const server = read('gateway/proxy/server.mjs');
  const closeStart = server.indexOf("res.on('close'");
  assert.ok(closeStart >= 0, 'missing res close handler');
  const closeBlock = server.slice(closeStart, server.indexOf('});', closeStart) + 3);

  assert.match(closeBlock, /stream\.cancel\(\);/);
});

// C++ service: authenticated owner, durable pipeline, single terminal

test('query service resolves the owner exclusively from the auth interceptor', () => {
  const service = read('server/src/ai_query_service.cpp');

  assert.match(service, /AuthInterceptor::currentUserId\(\)/);
  assert.match(service, /enriched_req\.set_user_id\(owner_id\)/);
  // The request-body user_id fallback must be gone.
  assert.doesNotMatch(service, /std::string user_id = request->user_id\(\);/);
  assert.doesNotMatch(service, /user_id = AuthInterceptor::currentUserId\(\);\s*\}/);
});

test('query service removed the Redis micro-dollar budget middleware path', () => {
  const service = read('server/src/ai_query_service.cpp');

  assert.doesNotMatch(service, /BudgetMiddleware::checkAndDeduct/);
  assert.match(service, /budget_repo_->reserve\(/);
});

test('query service reads explicit NEXUSAI_BUDGET_*_TOKENS environment quotas', () => {
  const service = read('server/src/ai_query_service.cpp');

  for (const name of [
    'NEXUSAI_BUDGET_GLOBAL_TOKENS',
    'NEXUSAI_BUDGET_USER_DAILY_TOKENS',
    'NEXUSAI_BUDGET_USER_MONTHLY_TOKENS',
    'NEXUSAI_BUDGET_SESSION_TOKENS',
  ]) {
    assert.ok(service.includes(name), `missing ${name}`);
  }
  assert.match(service, /usage\.estimated = true;/);
});

test('query service finalizes each run exactly once via an atomic guard', () => {
  const service = read('server/src/ai_query_service.cpp');

  assert.match(service, /finalizeDurableQuery/);
  assert.match(service, /compare_exchange_strong\(expected, true\)/);
  assert.match(service, /reserveBudgetOrReject/);
  assert.match(service, /ensureConversation/);
  assert.match(service, /appendMessageAutoSequence/);
  assert.match(service, /buildSystemContextFromPg/);
});

test('query stream emits the terminal event exactly once through an atomic flag', () => {
  const service = read('server/src/ai_query_service.cpp');

  assert.match(service, /std::atomic<bool> terminal_emitted\{false\};/);
  assert.match(service, /terminal_emitted\.compare_exchange_strong/);
  // Lower-layer terminal events are filtered in both relays.
  assert.ok(
    countMatches(service, /event\.event_type\(\) == "complete"/g) >= 1,
    'relay must filter lower-layer complete events',
  );
});

// Multi-agent handler: no terminal emission

test('multi-agent handler never emits terminal complete/error events', () => {
  const handler = read('server/src/multi_agent_handler.cpp');

  assert.equal(countMatches(handler, /set_event_type\("complete"\)/g), 0);
  assert.equal(countMatches(handler, /set_event_type\("error"\)/g), 0);
  // Terminal outcome is handed to the service layer instead.
  assert.match(handler, /tls_stream_result\.answer/);
  assert.match(handler, /tls_stream_result\.error/);
  assert.match(handler, /context->IsCancelled\(\)/);
});

// RpcServer: fail-closed dependency injection

test('rpc server owns the durable repositories and refuses partial startup', () => {
  const rpcServer = read('server/src/rpc_server.cpp');

  assert.match(rpcServer, /QueryDomainRepository/);
  assert.match(rpcServer, /PostgresBudgetRepository/);
  assert.match(rpcServer, /\*postgres_store_,\s*\*query_domain_repository_,\s*\*budget_repository_/);
  assert.doesNotMatch(rpcServer, /continuing without it/);
});

// P20: gateway converts request timeout_seconds into a gRPC deadline

test('proxy derives a gRPC deadline from timeout_seconds (P20)', () => {
  const server = read('gateway/proxy/server.mjs');
  assert.match(server, /function deadlineFromBody\(body\) \{/);
  assert.match(server, /body && body\.timeout_seconds/);
  // Both unary and streaming calls pass the derived deadline option; the
  // absent-field path leaves it unset (legacy byte-equivalent behavior).
  assert.match(server, /const options = deadline !== undefined \? \{ deadline \} : \{\};/);
  assert.match(server, /client\[grpcMethod\]\(body, metadata, options, \(err, response\)/);
  assert.match(server, /client\[grpcMethod\]\(body, metadata, options\);/);
});
