#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gpsp-rfu-test.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Compile the production implementation, replacing only hardware dependencies.
cp "$repo/rfu.c" "$tmp/rfu.c"
cp "$repo/tests/rfu/common.h" "$tmp/common.h"
cp "$repo/tests/rfu/queue.c" "$tmp/queue.c"

# CFLAGS/CC are intentional conventional compiler arguments, not data inputs.
for capacity in default 32 64; do
  define=
  if [ "$capacity" != default ]; then
    define="-DRFU_RX_QUEUE_SIZE=$capacity"
  fi
  ${CC:-cc} ${CFLAGS:--std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer} \
    $define "$tmp/queue.c" -o "$tmp/queue-test"
  "$tmp/queue-test"
done

for capacity in 3 65; do
  if ${CC:-cc} -std=c99 -DRFU_RX_QUEUE_SIZE="$capacity" \
      "$tmp/queue.c" -o "$tmp/invalid" >"$tmp/invalid.log" 2>&1; then
    echo "Unexpectedly accepted queue capacity $capacity" >&2
    exit 1
  fi
  grep -q 'RFU_RX_QUEUE_SIZE must be between 4 and 64' "$tmp/invalid.log"
done
echo "Invalid queue capacities rejected"
