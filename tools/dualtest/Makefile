# Test rig for the Dual Cable Link (needs the Miyoo Mini toolchain and
# qemu-arm). "make" builds the link test ROM and the two-console harness;
# run.sh runs a test, see dualtest.c.
TC      ?= /opt/miyoomini-toolchain/usr/bin/arm-linux
ROMFLAGS = -march=armv4t -marm -mfloat-abi=soft -ffreestanding -nostdlib -O2 -fno-pic

all: linktest.gba dualtest

linktest.gba: crt0.S linktest.c rom.ld
	$(TC)-gcc $(ROMFLAGS) -c crt0.S -o crt0.o
	$(TC)-gcc $(ROMFLAGS) -c linktest.c -o linktest.o
	$(TC)-ld -T rom.ld crt0.o linktest.o -o linktest.elf --no-warn-mismatch
	$(TC)-objcopy -O binary linktest.elf $@
	# 2MB: a 1MB ROM would be taken for a mirrored mini ROM (no dynarec)
	truncate -s 2M $@
	rm -f crt0.o linktest.o linktest.elf

dualtest: dualtest.c
	$(TC)-gcc -O2 -Wall -I../../libretro/libretro-common/include -o $@ $< -ldl -lpthread

clean:
	rm -f linktest.gba dualtest crt0.o linktest.o linktest.elf
