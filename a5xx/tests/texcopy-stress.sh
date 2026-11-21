#!/bin/sh
# Reproduce the a5xx texture-copy flake/hang.
# Run on the device from /tmp/tu5-test:
#   ITER=50 ./texcopy-stress.sh
#   BIGCLEAR=1 ITER=50 ./texcopy-stress.sh
#   VK_ICD_FILENAMES=.../texfix.json TU5_GFX_DEBUG=1 ./texcopy-stress.sh
set -u

cd "$(dirname "$0")"
ITER=${ITER:-50}
BIGCLEAR=${BIGCLEAR:-0}
LOG=${LOG:-/home/user/texcopy-stress-$(date +%H%M%S).log}
: > "$LOG"

mark() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; sync; }

mark "start iter=$ITER bigclear=$BIGCLEAR icd=${VK_ICD_FILENAMES:-system} debug=${TU5_GFX_DEBUG:-0}"

i=0
while [ "$i" -lt "$ITER" ]; do
   i=$((i + 1))
   if [ "$BIGCLEAR" = 1 ]; then
      timeout 30 ./tu5_bigclear_test >/tmp/big.o 2>&1
      r=$?
      mark "iter $i bigclear rc=$r $(tail -1 /tmp/big.o)"
      [ "$r" -ne 0 ] && break
   fi

   timeout 25 ./tu5_texcopy_test >/tmp/o 2>&1
   r=$?
   mark "iter $i texcopy rc=$r $(tail -1 /tmp/o)"
   if [ "$r" -ne 0 ]; then
      # rc=1 is the known zero-sample failure; keep going but log it.
      # 124/137 means the test did not finish (hang/kill).
      case "$r" in
         1) : ;;
         *) mark "STOP rc=$r"; break ;;
      esac
   fi
done
mark "DONE iterations=$i"
