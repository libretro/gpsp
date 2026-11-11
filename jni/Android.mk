LOCAL_PATH := $(call my-dir)

CORE_DIR     := $(LOCAL_PATH)/..

CORE_LDLIBS  :=
CPU_ARCH     :=
HAVE_DYNAREC :=

COREFLAGS := -DINLINE=inline -D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565

ifeq ($(TARGET_ARCH),arm)
   COREFLAGS += -DARM_ARCH -DMMAP_JIT_CACHE
   CPU_ARCH := arm
   HAVE_DYNAREC := 1
else ifeq ($(TARGET_ARCH),arm64)
   COREFLAGS += -DARM64_ARCH -DMMAP_JIT_CACHE
   CPU_ARCH := arm64
   HAVE_DYNAREC := 1
else ifeq ($(TARGET_ARCH),x86)
   COREFLAGS += -DMMAP_JIT_CACHE
   CPU_ARCH := x86_32
   HAVE_DYNAREC := 1
else ifeq ($(TARGET_ARCH),x86_64)
   COREFLAGS += -DMMAP_JIT_CACHE
   CPU_ARCH := x86_32
   HAVE_DYNAREC := 1
endif

ifeq ($(HAVE_DYNAREC),1)
  COREFLAGS += -DHAVE_DYNAREC
endif

include $(CORE_DIR)/Makefile.common

GIT_VERSION := "$(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION),"unknown")
   COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

# We do not use the stdlib++ on purpose, disable it to reduce dependencies.

# Dual Cable Link: the other GBAs (hidden instances B, C and D) are the whole
# core built again with -DGPSP_INSTANCE_B, linked into one object and copied
# three times with every global renamed gbab_*, gbac_* and gbad_* (as the
# Makefile does). Instance A gets dual.c, the session and the cable bus.
include $(CLEAR_VARS)
LOCAL_MODULE    := retro_b
LOCAL_SRC_FILES := $(SOURCES_C) $(SOURCES_ASM) $(SOURCES_CC)
LOCAL_CFLAGS    := $(COREFLAGS) $(INCFLAGS) -DGPSP_INSTANCE_B -Wno-unused-variable -Wno-unused-label
LOCAL_ARM_MODE  := arm
include $(BUILD_STATIC_LIBRARY)
GPSP_B_LIB := $(LOCAL_BUILT_MODULE)

# Inside retro_b's object folder, so that "ndk-build clean" removes them too.
GPSP_HIDDEN_DIR := $(TARGET_OBJS)/retro_b/hidden
GPSP_HIDDEN     := $(GPSP_HIDDEN_DIR)/gpsp_b.o $(GPSP_HIDDEN_DIR)/gpsp_c.o $(GPSP_HIDDEN_DIR)/gpsp_d.o

$(GPSP_HIDDEN_DIR)/gpsp_hidden.o: $(GPSP_B_LIB)
	@mkdir -p $(dir $@)
	$(LLVM_TOOLCHAIN_PREFIX)ld.lld -r --whole-archive $< -o $@

$(GPSP_HIDDEN_DIR)/gpsp_%.o: $(GPSP_HIDDEN_DIR)/gpsp_hidden.o
	$(LLVM_TOOLCHAIN_PREFIX)llvm-nm -g --defined-only $< | awk 'NF == 3 { print $$3 " gba$*_" $$3 }' > $@.syms
	$(LLVM_TOOLCHAIN_PREFIX)llvm-objcopy --redefine-syms=$@.syms $< $@

include $(CLEAR_VARS)
LOCAL_DISABLE_FATAL_LINKER_WARNINGS := true
LOCAL_MODULE    := retro
LOCAL_SRC_FILES := $(SOURCES_C) $(SOURCES_ASM) $(SOURCES_CC) $(CORE_DIR)/dual.c
LOCAL_CFLAGS    := $(COREFLAGS) $(INCFLAGS)
LOCAL_LDFLAGS   := -Wl,-version-script=$(CORE_DIR)/link.T -nostdlib++ $(GPSP_HIDDEN)
LOCAL_LDLIBS    := $(CORE_LDLIBS)
LOCAL_ARM_MODE  := arm
include $(BUILD_SHARED_LIBRARY)
$(LOCAL_BUILT_MODULE): $(GPSP_HIDDEN)
