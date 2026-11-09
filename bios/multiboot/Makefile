# Builds the MultiBoot block for the built-in BIOS and the C array gpSP
# patches it in with (../../bios_patch.h). Needs an arm-none-eabi gcc
# (devkitARM or the ARM GNU toolchain): make CROSS=/path/to/arm-none-eabi-
CROSS ?= /opt/devkitpro/devkitARM/bin/arm-none-eabi-
CFLAGS = -marm -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin -nostdlib \
         -fno-pic -Wall -Wextra

all: ../../bios_patch.h

multiboot.elf: multiboot.c multiboot.ld
	$(CROSS)gcc $(CFLAGS) -T multiboot.ld -o $@ multiboot.c

multiboot.bin: multiboot.elf
	$(CROSS)objcopy -O binary -j .text $< $@

entry.txt: multiboot.elf
	$(CROSS)nm $< | sed -n 's/^\([0-9a-f]*\) T swi_MultiBoot$$/\1/p' > $@
	$(CROSS)nm $< | sed -n 's/^\([0-9a-f]*\) T bios_BootHook$$/\1/p' >> $@

../../bios_patch.h: multiboot.bin entry.txt mkheader.py
	python3 mkheader.py multiboot.bin `cat entry.txt` > $@
