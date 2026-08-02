#!/bin/bash
# update.sh — Pull latest code, build, and install binary.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local/bin}"
BINARY="codebase-memory-mcp"

cd "$ROOT"

echo "📥 Pulling latest code..."
# git pull --ff-only

echo "🔨 Building..."
./scripts/build.sh --with-ui "$@"

echo "📦 Installing to $INSTALL_DIR/$BINARY"
mkdir -p "$INSTALL_DIR"
cp build/c/$BINARY "$INSTALL_DIR/$BINARY"

git restore .

echo "✅ Done. Run: $BINARY --help"
