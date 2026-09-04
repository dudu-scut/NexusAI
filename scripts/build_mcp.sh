#!/usr/bin/env bash
# Build an alternate build-mcp/ tree with -DENABLE_MCP=ON (defines
# AGENT_RPC_ENABLE_MCP for orchestrator/server) without touching run.sh.
# Usage: wsl -e bash scripts/build_mcp.sh [make targets...]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${HOME}/nexusai-deps/prefix"

export PKG_CONFIG_PATH="${PREFIX}/usr/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="${PREFIX}/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p "$ROOT/build-mcp"
cd "$ROOT/build-mcp"

cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_MCP=ON \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_BUILD_RPATH="$PREFIX/lib" \
    -DCMAKE_INSTALL_RPATH="$PREFIX/lib" || exit 1

make -j"$(nproc)" "$@"
exit $?
