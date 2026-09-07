import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../../..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');

test('MCP-off build graph does not require MCP targets or headers', () => {
  const rootCmake = read('CMakeLists.txt');
  const orchestratorCmake = read('orchestrator/CMakeLists.txt');
  const main = read('server/src/main.cpp');

  assert.match(rootCmake, /option\(ENABLE_MCP[^\n]+OFF\)/);
  // P7 (批次十一): the vector-routing tier moved to agent_rpc_common, so the
  // orchestrator no longer links agent_rpc_mcp and no longer defines the
  // AGENT_RPC_ENABLE_MCP layout macro (the dual-implementation / ODR hazard
  // class is gone). Assert the ABSENCE so a regression cannot silently
  // re-introduce the MCP coupling.
  assert.match(orchestratorCmake, /if\(ENABLE_MCP\)/);
  assert.doesNotMatch(
    orchestratorCmake,
    /target_link_libraries\(orchestrator[^)]*agent_rpc_mcp/s,
    'orchestrator must not link agent_rpc_mcp (P7 decoupling)',
  );
  assert.doesNotMatch(
    orchestratorCmake,
    /target_compile_definitions\(orchestrator[^)]*AGENT_RPC_ENABLE_MCP/s,
    'AGENT_RPC_ENABLE_MCP must not be defined for orchestrator (P7)',
  );
  assert.doesNotMatch(
    main,
    /#ifdef AGENT_RPC_ENABLE_MCP/,
    'main.cpp must not contain MCP-gated code (P7)',
  );
});

test('Docker build context excludes secrets and generated artifacts', () => {
  const dockerignore = read('.dockerignore');
  for (const entry of ['.env', 'certs/', 'node_modules/', 'build/', '.git/', '.superpowers/', 'logs/', 'pids/']) {
    assert.ok(dockerignore.split(/\r?\n/).includes(entry), `missing ${entry}`);
  }
  assert.match(read('backend/Dockerfile'), /postgresql-client/);
});

test('deprecated Envoy deployment artifacts are absent from supported paths', () => {
  for (const file of ['gateway/envoy.sh', 'gateway/envoy.yaml', 'gateway/nginx.conf']) {
    assert.equal(fs.existsSync(path.join(root, file)), false, `${file} should be removed`);
  }
  for (const file of ['README.md', 'CLAUDE.md', 'run.sh', 'frontend/vite.config.ts']) {
    assert.doesNotMatch(read(file), /Envoy|gRPC-Web/i);
  }
});
