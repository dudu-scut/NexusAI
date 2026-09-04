#!/usr/bin/env bash
# Helper: run the baseline/regression ctest group inside WSL with the
# controlled libpqxx/libpq runtime path and .env loaded.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="${HOME}/nexusai-deps/prefix/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -f "$ROOT/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT/.env"
    set +a
fi

# Defaults mirror .env.example so PostgresStore::fromEnvironment can build a
# DSN against the docker-compose postgres service when .env omits them.
export NEXUSAI_POSTGRES_HOST="${NEXUSAI_POSTGRES_HOST:-127.0.0.1}"
export NEXUSAI_POSTGRES_PORT="${NEXUSAI_POSTGRES_PORT:-5432}"
export NEXUSAI_POSTGRES_DATABASE="${NEXUSAI_POSTGRES_DATABASE:-nexusai}"
export NEXUSAI_POSTGRES_USER="${NEXUSAI_POSTGRES_USER:-nexusai}"
export NEXUSAI_POSTGRES_PASSWORD="${NEXUSAI_POSTGRES_PASSWORD:-nexusai-dev-password}"

cd "$ROOT/build"
ctest -R 'RedisServicesTest|DurableQueryPipelineTest|WorkflowControlContractTest' --output-on-failure "$@"
exit $?
