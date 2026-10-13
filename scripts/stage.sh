#!/usr/bin/env bash
# Rebuild and refresh the staged workshop. Staging serves dist/workshop
# directly (adamOS stagectl, port 8432), so a rebuild is live on reload.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/build.sh
echo "staged: https://controlplane.samellgass.com/staging?port=8432"
