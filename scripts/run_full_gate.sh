#!/usr/bin/env bash
# Full test gate: run every registered ctest suite inside WSL with the
# controlled libpqxx/libpq runtime path and .env loaded (mirrors the
# environment that run_test_group.sh / run_ctest_group.sh set up).
# Usage: wsl -e bash scripts/run_full_gate.sh [extra ctest args]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="${HOME}/nexusai-deps/prefix/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -f "$ROOT/.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ROOT/.env"
    set +a
fi

export NEXUSAI_POSTGRES_HOST="${NEXUSAI_POSTGRES_HOST:-127.0.0.1}"
export NEXUSAI_POSTGRES_PORT="${NEXUSAI_POSTGRES_PORT:-5432}"
export NEXUSAI_POSTGRES_DATABASE="${NEXUSAI_POSTGRES_DATABASE:-nexusai}"
export NEXUSAI_POSTGRES_USER="${NEXUSAI_POSTGRES_USER:-nexusai}"
export NEXUSAI_POSTGRES_PASSWORD="${NEXUSAI_POSTGRES_PASSWORD:-nexusai-dev-password}"

cd "$ROOT/build"
ctest --output-on-failure --timeout 300 "$@"
exit $?
