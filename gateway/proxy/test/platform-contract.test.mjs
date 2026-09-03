import { execFileSync } from 'node:child_process';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../../..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const serviceBlock = (compose, service) => {
  const start = compose.indexOf(`  ${service}:`);
  assert.ok(start >= 0, `missing Compose service block: ${service}`);
  const block = compose.slice(start);
  const nextService = block.search(/\n(?=  [a-z0-9_-]+:|networks:|volumes:|secrets:)/);
  return block.slice(0, nextService >= 0 ? nextService : block.length);
};

test('production compose uses the JSON proxy and service DNS', () => {
  const compose = read('docker-compose.yml');

  for (const service of ['postgres', 'redis', 'rpc-server', 'proxy', 'frontend']) {
    assert.match(compose, new RegExp(`^  ${service}:`, 'm'));
  }
  assert.doesNotMatch(compose, /^  migrate:/m);
  assert.match(compose, /GRPC_TARGET:\s*rpc-server:50051/);
  assert.doesNotMatch(compose, /envoy:/i);
  assert.doesNotMatch(compose, /host\.docker\.internal|172\.\d+\.\d+\.\d+/);
});

test('RPC Dockerfile preserves the repository source hierarchy', () => {
  const rpcDockerfile = read('docker/Dockerfile.rpc-server');

  assert.match(rpcDockerfile, /WORKDIR\s+\/src\s*\r?\nCOPY\s+\.\s+\./);
  assert.doesNotMatch(rpcDockerfile, /COPY\s+CMakeLists\.txt\s+\.\//);
  assert.doesNotMatch(rpcDockerfile, /COPY\s+proto\s+common\s+registry\s+a2a\s+a2a_adapter\s+orchestrator\s+server\s+client\s+\.\//);
});

test('RPC builder installs the nlohmann JSON development headers', () => {
  const rpcDockerfile = read('docker/Dockerfile.rpc-server');
  const builderAptInstall = rpcDockerfile.match(
    /FROM\s+\S+\s+AS\s+build[\s\S]*?\bRUN\s+apt-get update && apt-get install -y --no-install-recommends([\s\S]*?)\r?\n\s+&& rm -rf \/var\/lib\/apt\/lists\/\*/,
  );

  assert.ok(builderAptInstall, 'missing builder apt install block');
  assert.match(builderAptInstall[1], /(?:^|[\s\\])nlohmann-json3-dev(?:[\s\\]|$)/);
});

test('Vite forwards browser RPCs only to the local JSON proxy', () => {
  const vite = read('frontend/vite.config.ts');

  assert.match(vite, /target:\s*'http:\/\/localhost:8081'/);
  assert.doesNotMatch(vite, /Envoy|grpc-web/i);
});

test('run script manages a proxy PID without set -e unsafe postincrements', () => {
  const script = read('run.sh');

  assert.match(script, /proxy\.pid/);
  assert.match(script, /proxy\.log/);
  assert.doesNotMatch(script, /\(\([^\n]*\+\+\)\)/);
  assert.doesNotMatch(script, /\/mnt\/c\/Users\//);
});

test('run.sh is tracked as executable for the WSL command path', () => {
  const indexEntry = execFileSync('git', ['ls-files', '--stage', '--', 'run.sh'], {
    cwd: root,
    encoding: 'utf8',
  }).trim();
  const [mode] = indexEntry.split(/\s+/, 1);

  assert.equal(mode, '100755', 'run.sh must be tracked as executable for ./run.sh commands');
});

test('run.sh start-all delegates to the root Compose gateway without legacy runtime paths', () => {
  const script = read('run.sh');
  const startAllStart = script.indexOf('cmd_start_all() {');
  const usageStart = script.indexOf('usage() {', startAllStart);

  assert.ok(startAllStart >= 0, 'missing cmd_start_all');
  assert.ok(usageStart > startAllStart, 'missing usage section after cmd_start_all');
  const startAll = script.slice(startAllStart, usageStart);

  assert.match(startAll, /cmd_gateway(?:\s+"\$@")?/);
  assert.doesNotMatch(startAll, /cmd_(?:redis|start_mock_agent|start_orchestrator|start_proxy|start)\b/);
  assert.doesNotMatch(startAll, /GRPC_TARGET|localhost:50051|Envoy|gRPC-Web|WSL|Windows/i);
});

test('run.sh gateway and stop pin root Compose without volume teardown', () => {
  const script = read('run.sh');
  const gatewayStart = script.indexOf('cmd_gateway() {');
  const stopStart = script.indexOf('cmd_stop() {', gatewayStart);
  const setupStart = script.indexOf('cmd_setup() {', stopStart);

  assert.ok(gatewayStart >= 0 && stopStart > gatewayStart && setupStart > stopStart);
  const gateway = script.slice(gatewayStart, stopStart);
  const stop = script.slice(stopStart, setupStart);

  for (const section of [gateway, stop]) {
    assert.match(section, /docker compose -f "\$PROJECT_ROOT\/docker-compose\.yml"/);
    assert.doesNotMatch(section, /GATEWAY_COMPOSE|docker compose[^\n]*(?:--volumes|\s-v(?:\s|$))/);
  }
});

test('run.sh stop unconditionally tears down root Compose resources', () => {
  const script = read('run.sh');
  const stopStart = script.indexOf('cmd_stop() {');
  const setupStart = script.indexOf('cmd_setup() {', stopStart);

  assert.ok(stopStart >= 0 && setupStart > stopStart);
  const stop = script.slice(stopStart, setupStart);
  const dockerCleanupStart = stop.indexOf('if command -v docker');
  const dockerCleanupEnd = stop.indexOf('\n    fi', dockerCleanupStart);

  assert.ok(dockerCleanupStart >= 0, 'missing Docker/root Compose cleanup guard');
  assert.ok(dockerCleanupEnd > dockerCleanupStart, 'missing Docker/root Compose cleanup guard end');

  const dockerCleanup = stop.slice(dockerCleanupStart, dockerCleanupEnd);
  assert.match(
    dockerCleanup,
    /if command -v docker\s+&>\/dev\/null && \[ -f "\$PROJECT_ROOT\/docker-compose\.yml" \]; then/,
  );
  assert.match(dockerCleanup, /^\s+docker compose -f "\$PROJECT_ROOT\/docker-compose\.yml" down\s*$/m);
  assert.doesNotMatch(dockerCleanup, /\n\s+if\b/);
  assert.doesNotMatch(dockerCleanup, /ps\s+--format\s+json|["']running["']/);
});

test('WSL bootstrap refuses Windows mounts and documents its package checks', () => {
  const bootstrap = read('scripts/bootstrap-wsl.sh');

  assert.match(bootstrap, /\/mnt\//);
  for (const packageName of ['build-essential', 'cmake', 'pkg-config', 'libgrpc\+\+-dev', 'protobuf-compiler-grpc', 'libgtest-dev', 'librapidcheck-dev', 'nlohmann-json3-dev', 'postgresql-client', 'redis-server']) {
    assert.ok(bootstrap.includes(packageName), `missing package check: ${packageName}`);
  }
});

test('WSL bootstrap pins and verifies the Ubuntu 26.04 libpqxx workaround', () => {
  const bootstrap = read('scripts/bootstrap-wsl.sh');
  const run = read('run.sh');

  assert.match(bootstrap, /NEXUSAI_LIBPQXX_VERSION=\"8\.0\.1\"/);
  assert.match(bootstrap, /NEXUSAI_LIBPQXX_SOURCE_URL=.*github\.com\/jtv\/libpqxx/);
  assert.match(bootstrap, /NEXUSAI_LIBPQXX_SOURCE_SHA256=\"[0-9a-f]{64}\"/);
  assert.match(bootstrap, /sha256sum --check --strict/);
  assert.match(bootstrap, /curl --fail --location --proto '=https' --tlsv1\.2/);
  assert.match(bootstrap, /libpqxx_is_ubuntu_2604/);
  assert.match(bootstrap, /ensure_libpqxx_for_ubuntu_2604/);
  assert.match(bootstrap, /Refusing to overwrite an existing uncontrolled libpqxx prefix/);
  assert.ok(bootstrap.includes('/mnt|/mnt/*'), 'prefix validation must reject /mnt paths');
  assert.match(bootstrap, /libpqxx_system_version[\s\S]*dpkg-query[\s\S]*env -u PKG_CONFIG_PATH/);
  assert.match(bootstrap, /All documented Ubuntu packages are installed[\s\S]*command -v cmake/);
  assert.match(bootstrap, /\.nexusai-libpqxx/);
  assert.match(run, /controlled_libpqxx_prefix/);
  assert.match(run, /NEXUSAI_LIBPQXX_SOURCE_SHA256_PIN="24f878a1b4249035e4b6c07d49351506bf99f88df584d36bf198d58ebf293823"/);
  assert.match(run, /source_sha256=\$NEXUSAI_LIBPQXX_SOURCE_SHA256_PIN/);
  assert.match(run, /-DCMAKE_PREFIX_PATH=\$cmake_prefix_path/);
  assert.match(run, /PKG_CONFIG_PATH=/);
  assert.match(run, /CMAKE_BUILD_RPATH/);
});

test('WSL bootstrap provides Ubuntu packages and a Go fallback for PR1 setup dependencies', () => {
  const bootstrap = read('scripts/bootstrap-wsl.sh');
  const requiredPackages = bootstrap.match(/REQUIRED_PACKAGES=\(([\s\S]*?)\r?\n\)\r?\n\r?\nmissing=/);

  assert.ok(requiredPackages, 'missing REQUIRED_PACKAGES block');
  assert.doesNotMatch(requiredPackages[1], /(?:^|\s)grpcurl(?:\s|$)/, 'grpcurl is not an Ubuntu package');
  for (const [requirement, packageName] of [
    ['pg_config', 'libpq-dev'],
    ['pkg-config package libsodium', 'libsodium-dev'],
    ['libpqxx', 'libpqxx-dev'],
    ['grpcurl Go SDK', 'golang-go'],
  ]) {
    assert.match(requiredPackages[1], new RegExp(`(?:^|\\s)${packageName}(?:\\s|$)`), `missing Ubuntu package for ${requirement}: ${packageName}`);
  }

  const fallbackStart = bootstrap.indexOf('if ! command -v grpcurl');
  const dockerStart = bootstrap.indexOf('if ! command -v docker', fallbackStart);
  assert.ok(fallbackStart >= 0 && dockerStart > fallbackStart, 'missing grpcurl Go fallback before Docker checks');
  const fallback = bootstrap.slice(fallbackStart, dockerStart);

  for (const [requirement, check] of [
    ['sudo guard', /if ! command -v sudo[\s\S]*sudo is required/],
    ['temporary build directory', /mktemp -d/],
    ['Go install source', /GOBIN=.*go install github\.com\/fullstorydev\/grpcurl\/cmd\/grpcurl@latest/],
    ['controlled PATH install', /sudo install[^\n]*\/usr\/local\/bin\/grpcurl/],
    ['temporary cleanup', /rm\s+-rf[\s\S]*grpcurl_build_dir/],
  ]) {
    assert.match(fallback, check, `missing grpcurl fallback ${requirement}`);
  }
  assert.ok((fallback.match(/command -v grpcurl/g) ?? []).length >= 2, 'fallback must verify grpcurl is on PATH after installation');
});

test('Compose gates the JSON gateway and publishes local HTTP web traffic', () => {
  const compose = read('docker-compose.yml');
  const postgres = serviceBlock(compose, 'postgres');
  const rpcServer = serviceBlock(compose, 'rpc-server');
  const proxy = serviceBlock(compose, 'proxy');
  const frontend = serviceBlock(compose, 'frontend');

  assert.ok(fs.existsSync(path.join(root, 'docker/Dockerfile.rpc-server')));
  assert.match(compose, /dockerfile:\s*docker\/Dockerfile\.rpc-server/);
  assert.match(rpcServer, /healthcheck:/);
  const rpcDockerfile = read('docker/Dockerfile.rpc-server');
  assert.doesNotMatch(rpcServer, /kill\s+-0\s+1/);
  assert.match(rpcServer, /test:\s+\[\"CMD-SHELL\",\s+\"nc -z -w 1 127\.0\.0\.1 50051\"\]/);
  assert.match(rpcDockerfile, /netcat-openbsd/);
  assert.match(proxy, /depends_on:\s*\n\s+rpc-server:\s*\n\s+condition:\s+service_healthy/);
  assert.match(proxy, /healthcheck:/);
  assert.match(frontend, /depends_on:\s*\n\s+proxy:\s*\n\s+condition:\s+service_healthy/);
  assert.match(frontend, /healthcheck:/);
  assert.match(postgres, /\.\/\.nexusai-data\/postgres:\/var\/lib\/postgresql\/data/);
  assert.doesNotMatch(compose, /postgres-data:/);
  assert.match(frontend, /ports:\s*\n\s+-\s*["']?8080:8080/);
  assert.match(frontend, /http:\/\/127\.0\.0\.1:8080\/health/);
  assert.doesNotMatch(frontend, /ports:[\s\S]*8081:8081/);
  assert.match(compose, /GRPC_TARGET:\s*rpc-server:50051/);
  assert.doesNotMatch(compose, /secrets:|frontend_tls|certs\/dev|8443/);
  assert.doesNotMatch(compose, /envoy:/i);
});

test('Local Nginx serves HTTP and forwards unary and streaming JSON RPCs', () => {
  const nginx = read('frontend/nginx.conf');

  assert.match(nginx, /listen\s+8080/);
  assert.doesNotMatch(nginx, /listen\s+8443|\bssl\b|ssl_certificate|frontend_tls|\/run\/secrets/);
  assert.match(nginx, /location\s+\/agent_communication\./);
  assert.match(nginx, /proxy_pass\s+http:\/\/proxy:8081/);
  assert.match(nginx, /proxy_http_version\s+1\.1/);
  assert.match(nginx, /proxy_buffering\s+off/);
  assert.match(nginx, /proxy_read_timeout\s+300s/);
  assert.match(nginx, /proxy_set_header X-Forwarded-Proto http/);
  assert.doesNotMatch(nginx, /envoy|grpc-web/i);
});

test('Node workspaces expose the required scripts and keep lockfile roots aligned', () => {
  for (const directory of ['gateway/proxy', 'frontend']) {
    const packageJson = JSON.parse(read(`${directory}/package.json`));
    const lockJson = JSON.parse(read(`${directory}/package-lock.json`));
    for (const script of ['test', 'typecheck', 'lint', 'build']) {
      assert.equal(typeof packageJson.scripts?.[script], 'string', `${directory} missing ${script} script`);
    }
    assert.equal(lockJson.packages?.['']?.name, packageJson.name, `${directory} lockfile root name drifted`);
  }
});

test('WSL bootstrap validates Docker Desktop integration without frontend TLS setup', () => {
  const bootstrap = read('scripts/bootstrap-wsl.sh');

  assert.match(bootstrap, /docker\s+info/);
  assert.match(bootstrap, /docker\s+compose\s+version/);
  assert.match(bootstrap, /127\.0\.0\.1:8080/);
  assert.doesNotMatch(bootstrap, /certs\/dev|FRONTEND_TLS|subjectAltName|openssl\s+(?:verify|x509|req)/);
});

test('Default CMake keeps MCP opt-in without global compiler flags', () => {
  const rootCmake = read('CMakeLists.txt');
  const testsCmake = read('tests/CMakeLists.txt');

  assert.match(rootCmake, /cmake_minimum_required\(VERSION\s+3\.20\)/);
  assert.match(rootCmake, /set\(CMAKE_CXX_STANDARD\s+20\)/);
  assert.match(rootCmake, /option\(ENABLE_MCP[^\n]+OFF\)/);
  assert.doesNotMatch(rootCmake, /CMAKE_CXX_FLAGS/);
  assert.match(testsCmake, /if\(ENABLE_MCP\)[\s\S]*test_mcp_integration[\s\S]*endif\(\)/);
  assert.match(testsCmake, /if\(ENABLE_MCP\)[\s\S]*test_rag_mcp_properties[\s\S]*endif\(\)/);
});

test('run.sh uses portable Bash paths and cleans failed background services', () => {
  const script = read('run.sh');

  assert.match(script, /^#!\/usr\/bin\/env bash/m);
  assert.match(script, /trap\s+/);
  assert.match(script, /wait\s+/);
  assert.match(script, /proxy\.pid/);
  assert.match(script, /proxy\.log/);
  assert.doesNotMatch(script, /npm\s+install/);
  assert.doesNotMatch(script, /\(\([^\n]*\+\+\)\)/);
  assert.doesNotMatch(script, /\/mnt\/c\/Users\//);
});

test('run.sh setup aggregates the complete PR1 dependency inventory', () => {
  const script = read('run.sh');
  const setupStart = script.indexOf('cmd_setup() {');
  const dependencyStart = script.indexOf('check_development_package() {');
  const commandCheckStart = script.indexOf('cmd_require_dependencies() {');
  assert.ok(setupStart >= 0 && dependencyStart > setupStart && commandCheckStart > dependencyStart);
  const setup = script.slice(setupStart, commandCheckStart);
  const proxyStart = script.indexOf('cmd_start_proxy() {', commandCheckStart);
  assert.ok(proxyStart > dependencyStart);
  const dependencies = script.slice(dependencyStart, proxyStart);
  assert.doesNotMatch(setup, /check_wsl_ready\s*\|\|\s*!\s*cmd_require_dependencies/);
  assert.match(setup, /if\s+!\s+cmd_require_dependencies[\s\S]*return 1/);
  assert.match(dependencies, /local\s+-a\s+missing=\(\)/);
  assert.match(dependencies, /printf[\s\S]*missing/);
  for (const check of [
    /check_wsl_ready/, /docker/, /Docker Compose v2/, /docker\s+compose\s+version/, /cmake/, /3\.20/,
    /g\+\+/, /-std=c\+\+20/, /protoc/, /grpc_cpp_plugin/, /dpkg-query/, /gtest/i, /rapidcheck/i,
    /libcurl|curl-config/i, /openssl/i, /libsodium|sodium/i, /libpqxx|pqxx/i,
    /redis-server/, /redis-cli/, /psql/, /pg_config/, /node/, /npm/, /python3/, /grpcurl/
  ]) {
    assert.match(dependencies, check, `missing setup dependency check: ${check}`);
  }
});
test('Frontend declares Vite environment types used by the JSON client', () => {
  const envFile = path.join(root, 'frontend/src/env.d.ts');

  assert.ok(fs.existsSync(envFile), 'frontend/src/env.d.ts is required');
  const source = fs.readFileSync(envFile, 'utf8');
  assert.match(source, /vite\/client/);
  assert.match(source, /VITE_API_BASE/);
});

test('Frontend Docker healthcheck probes its local HTTP endpoint on IPv4 loopback', () => {
  const compose = read('docker-compose.yml');
  const frontend = serviceBlock(compose, 'frontend');

  assert.match(frontend, /http:\/\/127\.0\.0\.1:8080\/health/);
  assert.doesNotMatch(frontend, /https?:\/\/localhost:8443\/health/);
});

test('Local Compose state and browser endpoint are documented', () => {
  const env = read('.env.example');
  const gitignore = read('.gitignore');

  assert.match(env, /FRONTEND_URL=http:\/\/127\.0\.0\.1:8080/);
  assert.doesNotMatch(env, /FRONTEND_TLS|certs\/dev/);
  assert.match(gitignore, /^\.nexusai-data\/$/m);
});

test('rpc-server runs startup migrations itself; no separate migrate service exists', () => {
  const compose = read('docker-compose.yml');
  const rpcServer = serviceBlock(compose, 'rpc-server');

  // The standalone migrate service was removed; rpc-server owns its startup
  // migrations and must see canonical Compose DNS for PostgreSQL.
  assert.doesNotMatch(compose, /^  migrate:/m);
  assert.match(rpcServer, /NEXUSAI_POSTGRES_HOST:\s*postgres/);
  assert.match(rpcServer, /NEXUSAI_POSTGRES_PORT:\s*"5432"/);
  assert.match(rpcServer, /NEXUSAI_POSTGRES_DATABASE:/);
  assert.match(rpcServer, /NEXUSAI_POSTGRES_USER:/);
  assert.match(rpcServer, /NEXUSAI_POSTGRES_PASSWORD:/);
  assert.match(rpcServer, /NEXUSAI_MIGRATIONS_DIR:/);
  assert.doesNotMatch(rpcServer, /\bpsql\b|\.\/sql|\/sql/);

  // server/src/main.cpp must apply migrations before the server listens, and
  // a failed migration must abort startup instead of serving unmigrated state.
  const main = read('server/src/main.cpp');
  assert.match(main, /agent_rpc::db::MigrationRunner migration_runner\{postgres_store\};/);
  const migrateCall = main.indexOf('migration_runner.migrate(resolveMigrationDirectory())');
  const serverStart = main.indexOf('server.start()');
  assert.ok(migrateCall >= 0, 'main.cpp must invoke migration_runner.migrate');
  assert.ok(serverStart >= 0, 'main.cpp must start the RPC server');
  assert.ok(migrateCall < serverStart, 'migrations must run before the server listens');
  assert.match(main, /RPC startup migration failed/);
});

test('migration packaging carries libpqxx, db_migrate, and exactly baseline migrations', () => {
  const dockerfile = read('docker/Dockerfile.rpc-server');
  const compose = read('docker-compose.yml');
  assert.match(dockerfile, /libpqxx-dev/);
  assert.match(dockerfile, /libpqxx-\d+\.\d+/);
  assert.match(dockerfile, /COPY --from=build \/build\/db\/db_migrate \/usr\/local\/bin\/db_migrate/);
  assert.match(dockerfile, /COPY db\/migrations \/usr\/local\/share\/nexusai\/migrations/);
  assert.match(compose, /dockerfile:\s*docker\/Dockerfile\.rpc-server/);
  const migrations = fs.readdirSync(path.join(root, 'db/migrations')).filter((name) => /^V\d+__.*\.sql$/.test(name));
  assert.equal(migrations.length, 13);
  for (let version = 1; version <= 13; version += 1) {
    const prefix = `V${String(version).padStart(3, '0')}__`;
    assert.ok(migrations.some((name) => name.startsWith(prefix)), `missing migration ${prefix}`);
  }
});

test('CMake resolves libpqxx through config targets or pkg-config fallback', () => {
  const rootCmake = read('CMakeLists.txt');
  const resolver = read('cmake/resolve_libpqxx_target.cmake');
  assert.match(rootCmake, /find_package\(libpqxx CONFIG QUIET\)/);
  assert.match(resolver, /pkg_check_modules\(AGENT_RPC_LIBPQXX REQUIRED IMPORTED_TARGET libpqxx\)/);
  assert.match(resolver, /PkgConfig::AGENT_RPC_LIBPQXX/);
});

// P15 P0(c): the generic proxy forwards OrchestrationService RPCs

test('proxy forwards OrchestrationService RPCs (ExecutePlan/ReplayQuery/ExportConversation)', () => {
  const server = read('gateway/proxy/server.mjs');
  // The OrchestrationService client is registered at startup (no allowlist).
  assert.match(server, /clients\['agent_communication\.OrchestrationService'\]/);
  // Generic dynamic forwarding maps any service/method through the
  // registered client via camelCase conversion ¡ª ExecutePlan/ReplayQuery/
  // ExportConversation all reach the server through unaryCall/streamCall.
  assert.match(server, /const grpcMethod = methodName\[0\]\.toLowerCase\(\) \+ methodName\.slice\(1\);/);
  assert.match(server, /function unaryCall\(serviceName, methodName, body, metadata\)/);
  assert.match(server, /function streamCall\(serviceName, methodName, body, metadata, res\)/);
});
