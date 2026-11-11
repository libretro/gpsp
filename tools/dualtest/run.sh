#!/bin/sh
# Builds the harness and runs it on the gpSP core under qemu-arm.
#   run.sh [debug] ROM FRAMES [latency_ms] [solo_frames]
# "debug" links a core whose cable bus prints what it does (stderr).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE_DIR=$HERE/../..
TC=/opt/miyoomini-toolchain/usr/bin/arm-linux
SYSROOT=/opt/miyoomini-toolchain/arm-miyoomini-linux-gnueabihf/sysroot
OUT=${TMPDIR:-/tmp}

CORE=$CORE_DIR/gpsp_plus_libretro.so
if [ "$1" = "debug" ]; then
  shift
  CFLAGS="-fPIC -Ofast -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -I$CORE_DIR/libretro -I$CORE_DIR/libretro/libretro-common/include -I$CORE_DIR \
    -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ -DINLINE=inline \
    -DHAVE_DYNAREC -DARM_ARCH -DMMAP_JIT_CACHE -DFRONTEND_SUPPORTS_RGB565 -O3 -DNDEBUG -DDUAL_DEBUG"
  $TC-gcc $CFLAGS -c $CORE_DIR/dual.c -o $OUT/dual_dbg.o
  OBJS=$(cd $CORE_DIR && make -s -p platform=miyoomini -q 2>/dev/null | sed -n 's/^OBJECTS := //p' | head -1)
  (cd $CORE_DIR && $TC-gcc -fPIC -shared -Wl,--version-script=link.T -Wl,--no-undefined \
     -o $OUT/gpsp_dbg.so $OBJS $OUT/dual_dbg.o gpsp_b.o gpsp_c.o gpsp_d.o -lm -lpthread)
  CORE=$OUT/gpsp_dbg.so
fi

make -s -C $HERE
cd $HERE
exec qemu-arm -L $SYSROOT ./dualtest $CORE "$@"
