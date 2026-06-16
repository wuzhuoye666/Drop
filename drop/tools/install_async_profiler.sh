#!/bin/bash
# Download async-profiler binary for the Drop agent.
# This script is called during Docker build to fetch async-profiler.
set -euo pipefail

AP_VERSION="${AP_VERSION:-3.0}"
AP_URL="https://github.com/async-profiler/async-profiler/releases/download/v${AP_VERSION}/async-profiler-${AP_VERSION}-linux-x64.tar.gz"
AP_DIR="/opt/async-profiler"

echo "Downloading async-profiler v${AP_VERSION}..."
mkdir -p "${AP_DIR}"

# Try GitHub first, then Maven Central as fallback
if wget -q --timeout=60 -L "${AP_URL}" -O /tmp/ap.tar.gz 2>/dev/null; then
    echo "Downloaded from GitHub"
elif wget -q --timeout=60 \
    "https://repo1.maven.org/maven2/one/profiler/async-profiler/${AP_VERSION}/async-profiler-${AP_VERSION}-linux-x64.tar.gz" \
    -O /tmp/ap.tar.gz 2>/dev/null; then
    echo "Downloaded from Maven Central"
else
    echo "WARNING: Could not download async-profiler. Java profiling will not work."
    echo "You can manually install it to ${AP_DIR} and re-run the agent."
    exit 0
fi

tar xzf /tmp/ap.tar.gz -C "${AP_DIR}" --strip-components=1
chmod +x "${AP_DIR}/bin/asprof"
rm -f /tmp/ap.tar.gz
echo "async-profiler installed to ${AP_DIR}"
"${AP_DIR}/bin/asprof" --version || true
