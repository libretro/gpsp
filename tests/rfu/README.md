# RFU receive-queue regression test

Run `sh tests/rfu/run.sh` with GCC or Clang. The default flags enable AddressSanitizer
and UndefinedBehaviorSanitizer. `CC` and `CFLAGS` may override the compiler/flags.
No ROM, BIOS, save, frontend or network connection is needed.

The runner copies the production `rfu.c` to a temporary directory alongside a
minimal hardware/frontend shim. It injects RFU packets into `rfu_net_receive` and
consumes them through the actual receive command. The same checks run with the
unchanged default capacity (4), experimental capacity 32, and upper bound 64:

- Eight packets arriving before any game read: four delivered/four dropped at
  the default capacity; all eight delivered at the larger capacities.
- FIFO order, bounded overflow, queue drain/reuse and peak/drop counters in both
  directions; the existing host-packet liveness ACK behavior is preserved.
- Diagnostic callback once per 300 RFU frame updates after activity, and no
  callback when all counters are zero. The shim does not test frontend logging.
- Existing simulated host-no-response and overall timeout results.
- Compile-time rejection of capacities outside 4–64.

Full-core experiments can select a capacity with
`make CODE_DEFINES=-DRFU_RX_QUEUE_SIZE=32` in addition to the platform's usual
build arguments. Clean between capacity changes: Make does not track flag changes.
The default remains four packets; player slots and the wire format are unchanged.

The counters are cumulative for the loaded core's lifetime, including device
resets, and are reported through the libretro INFO logger. These tests demonstrate
queue behavior under an injected burst. They do not establish that queue overflow
causes any particular game's disconnect, validate a larger default, or prove
frontend counter output, repeated trades or complete multiplayer battles.
