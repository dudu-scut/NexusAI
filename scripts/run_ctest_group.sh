#!/usr/bin/env bash
# Helper: run an arbitrary ctest regex group inside WSL with the controlled
# libpqxx/libpq runtime path and .env loaded.
# Usage: wsl -e bash scripts/run_ctest_group.sh 'TestA|TestB' [extra ctest args]
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

# Regex may come via CTEST_REGEX env var (robust against shell quoting
# issues when invoked from PowerShell/wsl) or as the first argument.
REGEX="${CTEST_REGEX:-${1:-}}"
if [ -z "$REGEX" ]; then
    echo "usage: CTEST_REGEX='A|B' run_ctest_group.sh [regex] [extra args]" >&2
    exit 2
fi
shift || true

cd "$ROOT/build"
ctest -R "$REGEX" --output-on-failure "$@"
exit $?
