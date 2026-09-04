#!/usr/bin/env bash
# Helper: run an arbitrary ctest regex group against an alternative build
# directory (default: build-mcp, produced by scripts/build_mcp.sh with
# -DENABLE_MCP=ON). Same env wiring as run_ctest_group.sh.
# Usage: CTEST_REGEX='A|B' wsl -e bash scripts/run_ctest_group_mcp.sh [extra ctest args]
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

BUILD_DIR="${BUILD_DIR:-$ROOT/build-mcp}"
if [ ! -d "$BUILD_DIR" ]; then
    echo "build dir not found: $BUILD_DIR (run scripts/build_mcp.sh first)" >&2
    exit 2
fi

REGEX="${CTEST_REGEX:-${1:-}}"
if [ -z "$REGEX" ]; then
    echo "usage: CTEST_REGEX='A|B' run_ctest_group_mcp.sh [regex] [extra args]" >&2
    exit 2
fi
shift || true

cd "$BUILD_DIR"
ctest -R "$REGEX" --output-on-failure "$@"
exit $?
