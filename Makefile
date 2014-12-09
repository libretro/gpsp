TARGET      := gpsp_libretro.so

CC  = gcc
AR  = psp-ar
STATIC_LINKING = 0

CFLAGS   += -fPIC -Werror-implicit-function-declaration
CFLAGS   += -DPC_BUILD -Wall -m32
CFLAGS   += -D__LIBRETRO__


ifeq ($(DEBUG), 1)
OPTIMIZE	      := -O0 -g
OPTIMIZE_SAFE  := -O0 -g
else
OPTIMIZE	      := -O3
OPTIMIZE_SAFE  := -O2
endif


OBJS := main.o
OBJS += cpu.o
OBJS += memory.o
OBJS += video.o
OBJS += input.o
OBJS += sound.o
OBJS += cpu_threaded.o

OBJS += x86/x86_stub.o
OBJS += cheats.o
OBJS += zip.o

OBJS += libretro.o



ASFLAGS = $(CFLAGS)
INCDIRS := -I.
LDFLAGS += -shared -m32 -Wl,--no-undefined -Wl,--version-script=link.T
LDLIBS  += -lz


all: $(TARGET)

$(TARGET): $(OBJS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $@ $(OBJS)
else
	$(CC) -o $@ $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS)
endif

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS) $(OPTIMIZE) $(INCDIRS)

cpu.o: cpu.c
	$(CC) -c -o $@ $< $(CFLAGS) -Wno-unused-variable -Wno-unused-label $(OPTIMIZE_SAFE) $(INCDIRS)

%.o: %.S
	$(CC) -c -o $@ $< $(ASFLAGS) $(OPTIMIZE)

clean:
#	rm -f main.o cpu.o memory.o video.o input.o sound.o cpu_threaded.o x86_stub.o cheats.o zip.o libretro.o
	rm -f $(OBJS)
	rm -f $(TARGET)

.PHONY: $(TARGET) clean