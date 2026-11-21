#!/bin/sh
# Copy the freshly built Turnip and the compute smoke test to the phone and
# run the test against the /tmp copy (does not touch the installed driver).
#
# Usage: ./run-on-device.sh [user@host]
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$HERE/../build-a5xx}
HOST=${1:-${WHYRED_HOST:-}}
[ -n "$HOST" ] || { echo "usage: $0 user@host" >&2; exit 2; }
PORT=${PORT:-22}
DEST=/tmp/tu5-test

SO="$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so"
if [ ! -f "$SO" ]; then
   echo "missing $SO" >&2
   exit 1
fi

ssh -p "$PORT" "$HOST" "mkdir -p $DEST"
scp -P "$PORT" "$SO" "$HERE/tu5_test" "$HOST:$DEST/"

ssh -p "$PORT" "$HOST" "cat > $DEST/freedreno_a5xx.json <<'EOF'
{
   \"file_format_version\": \"1.0.0\",
   \"ICD\": {
      \"library_path\": \"$DEST/libvulkan_freedreno.so\",
      \"api_version\": \"1.0.0\"
   }
}
EOF"

ssh -p "$PORT" "$HOST" "VK_ICD_FILENAMES=$DEST/freedreno_a5xx.json VK_DRIVER_FILES=$DEST/freedreno_a5xx.json VK_LOADER_DEBUG=error $DEST/tu5_test"
