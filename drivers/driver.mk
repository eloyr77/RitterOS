# (67b) Building a RitterOS kernel module (a driver) outside RitterOS's
# tree, as sdk/ritteros.mk builds programs. A driver's Makefile sets a few
# variables and includes this file:
#
#   DRIVER   := example                 # the module: build/ARCH/example.mod
#   SRCS     := example.c               # its C files
#   RITTEROS := /path/to/a/RitterOS/checkout
#   include $(RITTEROS)/drivers/driver.mk
#
# `make` builds it for ARCH (x86_64, the default, or aarch64): compiled as
# the kernel's own modules are, against the driver interface only
# (drivers/include/: driver.h and its device class's header; nothing else
# of the kernel is in reach), one relocatable object (several C files are
# joined with `ld -r`), then checked: every name it leaves undefined must
# be declared by the interface (scripts/kexports.c), or the build fails.
# Install it by copying it to /system/ARCH/drivers/ on RitterOS; the
# kernel loads it at the next start when its hardware is there (`drivers`
# says so, or why not). The guide is drivers/CLAUDE.md.

ARCH ?= x86_64
ifeq ($(filter $(ARCH),x86_64 aarch64),)
$(error ARCH must be x86_64 or aarch64, not '$(ARCH)')
endif
ifeq ($(DRIVER),)
$(error set DRIVER (the module's name) before including driver.mk)
endif
RITTEROS_DIR := $(abspath $(RITTEROS))
ifeq ($(wildcard $(RITTEROS_DIR)/drivers/include/driver.h),)
$(error RITTEROS=$(RITTEROS) is not a RitterOS checkout (no drivers/include/driver.h))
endif

# Zig's clang and lld, as for the kernel: RitterOS's own copy if
# scripts/setup-toolchain.sh made one, else zig from PATH.
ZIG ?= $(firstword $(wildcard $(RITTEROS_DIR)/tools/zig/zig) zig)
ZIG_EXE := $(abspath $(shell command -v $(ZIG) 2>/dev/null))
ifeq ($(ZIG_EXE),)
$(error no zig: install Zig 0.17.0 (https://ziglang.org/download/) or set ZIG=/path/to/zig)
endif
ZIG_LIB := $(abspath $(shell $(ZIG_EXE) env | sed -n 's/^ *\.lib_dir = "\(.*\)",$$/\1/p'))
HOSTCC ?= cc

# The flags of RitterOS's own modules (its Makefile's MODULE_CFLAGS).
TARGET_x86_64  := x86_64-unknown-none-elf
TARGET_aarch64 := aarch64-unknown-none-elf
FLAGS_x86_64   := -mno-red-zone -mcmodel=kernel
FLAGS_aarch64  :=
DRIVER_CFLAGS  := -target $(TARGET_$(ARCH)) -fno-pic -fno-pie -mgeneral-regs-only $(FLAGS_$(ARCH)) \
                  -ffreestanding -nostdinc -isystem $(ZIG_LIB)/include -fno-stack-protector \
                  -O2 -std=c11 -Wall -Wextra -Werror -I$(RITTEROS_DIR)/drivers/include
DRIVER_HEADERS := $(wildcard $(RITTEROS_DIR)/drivers/include/*.h)

BUILD   ?= build
OUT     := $(BUILD)/$(ARCH)
MODULE  := $(OUT)/$(DRIVER).mod
OBJS    := $(patsubst %.c,$(OUT)/%.o,$(SRCS))
KEXPORTS := $(BUILD)/host/kexports

.PHONY: all clean
all: $(MODULE)

$(KEXPORTS): $(RITTEROS_DIR)/scripts/kexports.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -o $@ $<

$(OUT)/%.o: %.c $(DRIVER_HEADERS)
	@mkdir -p $(dir $@)
	$(ZIG_EXE) clang $(DRIVER_CFLAGS) $(CFLAGS) -c $< -o $@

$(MODULE): $(OBJS) $(KEXPORTS)
	@mkdir -p $(dir $@)
	$(if $(word 2,$(OBJS)),$(ZIG_EXE) ld.lld -r -o $@.o $(OBJS),cp $(OBJS) $@.o)
	$(KEXPORTS) check $@.o $(DRIVER_HEADERS) || { rm -f $@.o; exit 1; }
	mv $@.o $@

clean:
	rm -rf $(BUILD)
