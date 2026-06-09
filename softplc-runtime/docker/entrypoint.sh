#!/bin/sh
set -eu

# Lock memory and apply real-time scheduling when capabilities allow.
ulimit -l unlimited 2>/dev/null || true

exec "$@"
