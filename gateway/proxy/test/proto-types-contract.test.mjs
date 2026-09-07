/**
 * proto.ts drift guard.
 *
 * Parses the repository proto/ sources (single source of truth) and compares
 * the fields of the core messages on this link against the hand-written
 * TypeScript declarations in frontend/src/types/proto.ts. Field-level checks:
 * every proto field must exist with a compatible TS type, and no ghost TS
 * field may survive that the wire format does not define.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../../..');
const PROTO_DIR = path.join(root, 'proto');
const PROTO_TS = path.join(root, 'frontend/src/types/proto.ts');

// proto/ parsing

function parseProtoMessages(file) {
  const src = fs.readFileSync(path.join(PROTO_DIR, file), 'utf8');
  const messages = {};
  const msgRe = /message\s+(\w+)\s*\{([\s\S]*?)\n\}/g;
  let m;
  while ((m = msgRe.exec(src)) !== null) {
    const fields = [];
    const fieldRe = /^\s*(repeated\s+)?(map<[^>]+>|[\w.]+)\s+(\w+)\s*=\s*\d+\s*;/gm;
    let f;
    while ((f = fieldRe.exec(m[2])) !== null) {
      fields.push({
        repeated: Boolean(f[1]),
        type: f[2].replace(/\s+/g, ''),
        name: f[3],
      });
    }
    messages[m[1]] = fields;
  }
  return messages;
}

const protoMessages = {
  ...parseProtoMessages('ai_query.proto'),
  ...parseProtoMessages('user.proto'),
  ...parseProtoMessages('observability.proto'),
  ...parseProtoMessages('sharing.proto'),
  ...parseProtoMessages('orchestration.proto'),
  ...parseProtoMessages('user_experience.proto'),
  ...parseProtoMessages('agent_lifecycle.proto'),
  ...parseProtoMessages('common.proto'),
  // agent discovery link (plan editor dropdown / register flows)
  ...parseProtoMessages('agent_service.proto'),
};

// Minor defensive assertion: if the regex parser silently degrades
// (proto syntax change, encoding drift), the whole suite would otherwise
// compare nothing against nothing and stay green.
assert.ok(
  Object.keys(protoMessages).length >= 25,
  `proto regex parser produced only ${Object.keys(protoMessages).length} messages — parser drift`,
);

// proto.ts parsing

const tsSource = fs.readFileSync(PROTO_TS, 'utf8');

function parseTsInterface(name) {
  const start = tsSource.indexOf(`export interface ${name} {`);
  if (start === -1) return null;
  let i = tsSource.indexOf('{', start);
  const open = i;
  let depth = 0;
  for (; i < tsSource.length; i++) {
    if (tsSource[i] === '{') depth++;
    else if (tsSource[i] === '}') {
      depth--;
      if (depth === 0) break;
    }
  }
  const body = tsSource.slice(open + 1, i);
  const fields = {};
  for (const rawLine of body.split('\n')) {
    const line = rawLine.replace(/\/\/.*$/, '').trim();
    const fm = line.match(/^(\w+)\??:\s*(.+?)\s*;?$/);
    if (fm) fields[fm[1]] = fm[2].replace(/;$/, '').trim();
  }
  return fields;
}

// type compatibility

// proto scalar → acceptable TS surface (proxy transcodes longs:String,
// bytes → base64 string).
const STRING_LITERALS = /^'[^']*'(\s*\|\s*'[^']*')*$/;
const TS_EXPECTATIONS = {
  string: (ts) => /string/.test(ts) || STRING_LITERALS.test(ts.replace(/\s+/g, ' ').trim()),
  bool: (ts) => /boolean/.test(ts),
  int32: (ts) => /number/.test(ts),
  int64: (ts) => /number/.test(ts) || /string/.test(ts),
  double: (ts) => /number/.test(ts),
  float: (ts) => /number/.test(ts),
  bytes: (ts) => /string/.test(ts),
};

function tsTypeOk(protoField, tsType) {
  if (protoField.type.startsWith('map<')) {
    return /Record<\s*string\s*,\s*string\s*>/.test(tsType);
  }
  if (protoField.repeated && protoField.type === 'string') {
    return /string\s*\[\]/.test(tsType);
  }
  if (protoField.repeated) {
    return /\[\]/.test(tsType);
  }
  const check = TS_EXPECTATIONS[protoField.type];
  if (check) return check(tsType);
  // Message-typed field: any object-ish declaration is acceptable here;
  // nested messages are checked independently when they are core messages.
  return tsType.length > 0;
}

// messages on this link that must never drift
const CORE_MESSAGES = [
  // AI query link
  'AIQueryRequest',
  'AIQueryResponse',
  'AIStreamEvent',
  'SystemContext',
  'AgentPreference',
  'Artifact',
  // execute-plan link (U4 rerun / plan editor)
  'DAGNode',
  'DAGStructure',
  'ExecutePlanRequest',
  'ExecutePlanResponse',
  // agent discovery + metrics (plan editor dropdown / monitor views)
  'ServiceInfo',
  'AgentMetrics',
  'GetAgentsRequest',
  'GetAgentsResponse',
  // auth + role
  'LoginResponse',
  'RegisterResponse',
  'ValidateTokenResponse',
  // observability (CostRecord.estimated)
  'CostRecord',
  'TokenUsageRecord',
  'TraceSpan',
  'GetTraceDetailResponse',
  'GetCostReportResponse',
  // replay / export / share / template
  'ReplayQueryRequest',
  'ReplayQueryResponse',
  'ExportConversationRequest',
  'ExportConversationResponse',
  'ShareSessionRequest',
  'ShareSessionResponse',
  'ReadSharedConversationRequest',
  'ReadSharedConversationResponse',
  // sandbox / intervention / compare / autonomy / undo
  'SandboxQueryRequest',
  'SandboxQueryResponse',
  'InterventionResponseRequest',
  'InterventionResponseResponse',
  'CompareAgentsRequest',
  'CompareAgentsResponse',
  'SetAutonomyLevelRequest',
  'UndoActionRequest',
  'UndoActionResponse',
];

for (const name of CORE_MESSAGES) {
  test(`proto.ts interface ${name} matches proto/ fields`, () => {
    const protoFields = protoMessages[name];
    assert.ok(protoFields, `proto/ must define message ${name}`);
    // Minor defensive assertion: a message parsed to zero fields means the
    // field regex no longer matches the wire source — fail loudly instead of
    // vacuously passing the field loop.
    assert.ok(
      protoFields.length > 0,
      `proto/ message ${name} parsed with zero fields — parser drift`,
    );
    const tsFields = parseTsInterface(name);
    assert.ok(tsFields, `proto.ts must declare interface ${name}`);
    assert.ok(
      Object.keys(tsFields).length > 0,
      `proto.ts interface ${name} parsed with zero fields — parser drift`,
    );

    for (const pf of protoFields) {
      const tsType = tsFields[pf.name];
      assert.ok(
        tsType !== undefined,
        `${name}.${pf.name} (${pf.type}) missing from proto.ts`,
      );
      assert.ok(
        tsTypeOk(pf, tsType),
        `${name}.${pf.name}: proto type "${pf.type}" incompatible with TS type "${tsType}"`,
      );
    }

    // Ghost detection: TS fields that the wire format does not define.
    const protoNames = new Set(protoFields.map((f) => f.name));
    const ghosts = Object.keys(tsFields).filter((k) => !protoNames.has(k));
    assert.deepEqual(
      ghosts,
      [],
      `${name}: proto.ts declares fields absent from proto/: ${ghosts.join(', ')}`,
    );
  });
}

// Specific regression pins

test('LoginResponse carries the C3 role field', () => {
  const ts = parseTsInterface('LoginResponse');
  assert.ok(ts.role, 'LoginResponse.role must exist');
  assert.match(ts.role, /string/);
});

test('Logout messages mirror the Logout RPC (P22 batch)', () => {
  // LogoutRequest is deliberately field-free (the token rides the
  // authorization metadata), so it cannot join CORE_MESSAGES whose loop
  // asserts at least one field — pin it here instead.
  assert.ok(protoMessages.LogoutRequest, 'proto/ must define LogoutRequest');
  assert.ok(protoMessages.LogoutResponse, 'proto/ must define LogoutResponse');
  const tsRequest = parseTsInterface('LogoutRequest');
  assert.ok(tsRequest, 'proto.ts must declare LogoutRequest');
  assert.deepEqual(
    Object.keys(tsRequest),
    [],
    'LogoutRequest must stay field-free (token rides metadata)',
  );
  const tsResponse = parseTsInterface('LogoutResponse');
  assert.ok(tsResponse, 'proto.ts must declare LogoutResponse');
  assert.ok(tsResponse.status, 'proto.ts LogoutResponse.status must exist');
});

test('CostRecord carries the C3 estimated flag', () => {
  const ts = parseTsInterface('CostRecord');
  assert.ok(ts.estimated, 'CostRecord.estimated must exist');
  assert.match(ts.estimated, /boolean/);
});

test('AIQueryRequest carries the sandbox flag', () => {
  const ts = parseTsInterface('AIQueryRequest');
  assert.ok(ts.sandbox, 'AIQueryRequest.sandbox must exist');
  assert.match(ts.sandbox, /boolean/);
});

test('AIQueryRequest carries the plan_only flag (B1 two-phase)', () => {
  // Field-level drift guard for the U4 two-phase execution switch.
  const ts = parseTsInterface('AIQueryRequest');
  assert.ok(ts.plan_only, 'AIQueryRequest.plan_only must exist');
  assert.match(ts.plan_only, /boolean/);
});

test('AIStreamEvent exposes trace_summary/activity_json/intervention_required', () => {
  const ts = parseTsInterface('AIStreamEvent');
  for (const f of ['trace_summary', 'activity_json', 'intervention_required']) {
    assert.ok(ts[f], `AIStreamEvent.${f} must exist`);
  }
});
