# RitterOS build. All tools come from ./tools (see scripts/setup-toolchain.sh).
#
#   make                  build both architectures and the disk image
#   make run              boot it on x86-64 (make run ARCH=aarch64: on ARM)
#   make test             all automated tests, both architectures
#
# Both architectures share one disk image: the firmware picks BOOTX64.EFI or
# BOOTAA64.EFI from the EFI system partition, which loads
# /system/<arch>/kernel.elf from the RitterFS partition.

ARCH     ?= x86_64
ARCHES   := x86_64 aarch64
ZIG      := tools/zig/zig
# Zig's own headers (stdint.h, stddef.h, ...): next to RitterOS's zig, or
# wherever a zig given on the command line keeps them (ZIG=/path/to/zig, as
# the SDK's sdk/ritteros.mk passes it).
ZIG_LIB  := $(if $(filter tools/zig/zig,$(ZIG)),tools/zig/lib,$(shell $(ZIG) env | sed -n 's/^ *\.lib_dir = "\(.*\)",$$/\1/p'))

BUILD    := build
OUT      := $(BUILD)/$(ARCH)

# The release (year.month, a second one in a month year.month.N: VERSION)
# and the commit it is built from (a git checkout's, + when tracked files
# changed; none outside git, as RitterOS's build of itself), for the
# kernel's build/version.h.
VERSION_TEXT   := $(strip $(shell cat VERSION 2>/dev/null))
VERSION_COMMIT := $(if $(wildcard .git),$(shell git rev-parse --short HEAD 2>/dev/null)$(if $(shell git status --porcelain -uno 2>/dev/null),+))
VERSION_H      := $(BUILD)/version.h

# (62d) TOOLCHAIN=ritteros: user space, and since 62f the kernel, built by
# RitterOS's own cc, as and ld (built for this machine into build/host with
# HOSTCC); the default stays zig's clang and lld. A change of toolchain rebuilds everything (the stamp
# is newer than every object).
TOOLCHAIN ?= zig
# (not HOST: sdk/ritteros.mk's HOST, the machine to deploy to, reaches this
# Makefile on its command line)
HOST_TOOLS := $(BUILD)/host
HOSTCC    ?= cc
ifeq ($(TOOLCHAIN),ritteros)
USER_CC        := $(HOST_TOOLS)/cc -B $(HOST_TOOLS)
USER_LD        := $(HOST_TOOLS)/ld
USER_AR        := $(HOST_TOOLS)/ar
USER_TOOLS     := $(HOST_TOOLS)/cc $(HOST_TOOLS)/as $(HOST_TOOLS)/ld $(HOST_TOOLS)/ar
USER_SYSTEM_INC := user/bin/cc/include
ZIG_DEP        :=
else
USER_CC        := $(ZIG) clang
USER_LD        := $(ZIG) ld.lld
USER_AR        := $(ZIG) ar
USER_TOOLS     :=
USER_SYSTEM_INC := $(ZIG_LIB)/include
ZIG_DEP        := $(ZIG)
endif
# (63) The build's generators, in C since level 2 (scripts/*.c; the Python
# scripts they replace wrote the same files), built for the machine running
# the build (HOSTCC: the Mac's cc, or cc on RitterOS).
GENERATORS     := $(HOST_TOOLS)/bdf2c $(HOST_TOOLS)/libexports $(HOST_TOOLS)/exfat_upcase $(HOST_TOOLS)/mathconst \
                  $(HOST_TOOLS)/kexports
TOOLCHAIN_STAMP := $(BUILD)/toolchain-$(TOOLCHAIN)
ESP      := $(BUILD)/esp
# Installed as /system on RitterFS.
SYSTEM   := $(BUILD)/system
KERNEL   := $(SYSTEM)/$(ARCH)/kernel.elf
# GPT disk images, one per architecture (55a): FAT32 EFI system partition
# (its bootloader from $(ESP)) + RitterOS partition (RitterFS, with that
# architecture's part of $(SYSTEM) as /system)
DISK     := $(BUILD)/ritteros-$(ARCH).img
DISKS    := $(foreach a,$(ARCHES),$(BUILD)/ritteros-$(a).img)

# Kernel command line, e.g. `make run CMDLINE=crash=pf`. Stored on the ESP.
CMDLINE  ?=
CMDLINE_FILE := $(ESP)/ritteros/cmdline.txt

# --- architectures --------------------------------------------------------------

# x86-64: UEFI uses the Microsoft x64 calling convention; the kernel uses
# System V, has no red zone (interrupts push onto its stack) and lives in the
# top 2 GiB. QEMU emulates a q35 PC (TCG).
x86_64_EFI_TARGET    := x86_64-unknown-windows
x86_64_EFI_FLAGS     := -mno-red-zone -mno-stack-arg-probe
x86_64_KERNEL_TARGET := x86_64-unknown-none-elf
x86_64_KERNEL_FLAGS  := -mno-red-zone -mcmodel=kernel
x86_64_BOOT_NAME     := BOOTX64.EFI
x86_64_QEMU          := tools/local/bin/qemu-system-x86_64
x86_64_FW_CODE       := tools/local/share/qemu/edk2-x86_64-code.fd
x86_64_FW_VARS       := tools/local/share/qemu/edk2-i386-vars.fd
x86_64_MACHINE       := -machine q35 -m 4G

# AArch64: no red zone; UEFI and the kernel both use the standard AAPCS64.
# QEMU's generic ARM machine runs on the Mac's own cores (hardware
# virtualization); ramfb is a simple framebuffer the firmware can drive.
aarch64_EFI_TARGET    := aarch64-unknown-windows
aarch64_EFI_FLAGS     :=
aarch64_KERNEL_TARGET := aarch64-unknown-none-elf
aarch64_KERNEL_FLAGS  :=
aarch64_BOOT_NAME     := BOOTAA64.EFI
aarch64_QEMU          := tools/local/bin/qemu-system-aarch64
aarch64_FW_CODE       := tools/local/share/qemu/edk2-aarch64-code.fd
aarch64_FW_VARS       := tools/local/share/qemu/edk2-arm-vars.fd
aarch64_MACHINE       := -machine virt -accel hvf -cpu host -m 4G -device ramfb

ifeq ($(filter $(ARCH),$(ARCHES)),)
$(error ARCH must be one of: $(ARCHES))
endif

# The complete QEMU command for an architecture. The disk is an NVMe SSD;
# virtio-rng is a hardware random number generator (the firmware offers
# EFI_RNG_PROTOCOL); a USB keyboard hangs on an xHCI USB controller behind two
# hubs (as on many PCs, whose ports are connected through internal hubs).
qemu_cmd = $($(1)_QEMU) $($(1)_MACHINE) -nic none \
           -drive if=pflash,format=raw,readonly=on,file=$($(1)_FW_CODE) \
           -drive if=pflash,format=raw,file=$(BUILD)/$(1)/vars.fd \
           -drive if=none,id=disk0,format=raw,file=$(BUILD)/ritteros-$(1).img \
           -device nvme,drive=disk0,serial=RITTEROS0001,bootindex=0 \
           -device virtio-rng-pci \
           -device qemu-xhci,id=xhci -device usb-hub,bus=xhci.0,port=1 \
           -device usb-hub,bus=xhci.0,port=1.1 -device usb-kbd,bus=xhci.0,port=1.1.1

BOOT_EFI := $(ESP)/EFI/BOOT/$($(ARCH)_BOOT_NAME)

COMMON_CFLAGS := -ffreestanding -nostdinc -isystem $(ZIG_LIB)/include \
                 -Icommon -Iarch/$(ARCH) -fno-stack-protector \
                 -O2 -std=c11 -Wall -Wextra -Werror

# The bootloader is a UEFI application: a PE32+ image ("Windows" target).
EFI_CFLAGS  := -target $($(ARCH)_EFI_TARGET) -fshort-wchar $($(ARCH)_EFI_FLAGS) \
               $(COMMON_CFLAGS) -Iboot
EFI_LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib

# Programs: freestanding ELF executables for user mode, linked at 0x400000
# (user/user.ld), with RitterOS's own small library (user/lib). Unlike the
# kernel they may use FP/SIMD registers: the kernel saves them per program.
USER_CFLAGS  := -target $($(ARCH)_KERNEL_TARGET) -ffreestanding -nostdinc \
                -isystem $(USER_SYSTEM_INC) -Iuser/include -fno-pic -fno-pie \
                -fno-stack-protector -fno-asynchronous-unwind-tables \
                -O2 -std=c11 -Wall -Wextra -Werror
USER_LDFLAGS := -T user/user.ld -nostdlib -static -z max-page-size=4096 -z norelro

# The kernel is a freestanding ELF executable. It avoids SSE/FPU (x86) and
# SIMD (ARM) registers so that exception handlers need not save them.
# (Its headers are the toolchain's own, as the programs': with zig the same
# as COMMON_CFLAGS'.)
KERNEL_CFLAGS  := -target $($(ARCH)_KERNEL_TARGET) -fno-pic -fno-pie -mgeneral-regs-only \
                  $($(ARCH)_KERNEL_FLAGS) -g $(subst $(ZIG_LIB)/include,$(USER_SYSTEM_INC),$(COMMON_CFLAGS)) -Ikernel \
                  -Idrivers/include -DRITTEROS_KERNEL
# (67b) A module sees only the driver interface: drivers/include/ is its
# one include folder (and the toolchain's freestanding headers).
MODULE_CFLAGS  := -target $($(ARCH)_KERNEL_TARGET) -fno-pic -fno-pie -mgeneral-regs-only \
                  $($(ARCH)_KERNEL_FLAGS) -ffreestanding -nostdinc -isystem $(USER_SYSTEM_INC) \
                  -fno-stack-protector -O2 -std=c11 -Wall -Wextra -Werror -Idrivers/include
KERNEL_LDFLAGS := -T kernel/linker.ld -nostdlib -static -z max-page-size=4096

# --- sources ---------------------------------------------------------------------

# Glyph tables generated from the X11 misc-fixed fonts (public domain).
GEN         := $(BUILD)/gen
FONT_C      := $(GEN)/font_data.c
FONTS       := regular=fonts/9x15.bdf bold=fonts/9x15B.bdf

ARCH_DIR    := arch/$(ARCH)
SHARED_BOOT_SRCS   := $(wildcard boot/*.c) $(filter-out common/aes.c common/ip6text.c,$(wildcard common/*.c))
# (67a) The table the module loader links modules with: (67b) every
# function the driver interface's headers declare.
DRIVER_HEADERS := $(wildcard drivers/include/*.h)
EXPORTS_C   := $(GEN)/exports.c
SHARED_KERNEL_SRCS := $(wildcard kernel/*.c) $(wildcard common/*.c) $(FONT_C) $(EXPORTS_C)
BOOT_SRCS   := $(SHARED_BOOT_SRCS) $(wildcard $(ARCH_DIR)/boot_arch.c $(ARCH_DIR)/uart.c)
KERNEL_SRCS := $(SHARED_KERNEL_SRCS) \
               $(filter-out $(ARCH_DIR)/boot_arch.c,$(wildcard $(ARCH_DIR)/*.c)) \
               $(wildcard $(ARCH_DIR)/*.S)
BOOT_OBJS   := $(patsubst %,$(OUT)/boot/%.obj,$(BOOT_SRCS))
KERNEL_OBJS := $(patsubst %,$(OUT)/kernel/%.o,$(KERNEL_SRCS))
HEADERS     := $(wildcard boot/*.h common/*.h kernel/*.h $(ARCH_DIR)/*.h) $(DRIVER_HEADERS)
# (67a) Kernel modules: drivers/*.c become /system/<arch>/drivers/*.mod,
# each one relocatable object compiled as the kernel is (without debug
# information: the loader reads only what runs). (37g-5a) drivers/NAME_*.c
# and drivers/*.h are parts a driver includes (amdgpu_atom.c), no modules.
DRIVER_PARTS := $(wildcard drivers/*_*.c drivers/*.h)
DRIVER_SRCS := $(filter-out $(DRIVER_PARTS),$(wildcard drivers/*.c))
MODULES     := $(patsubst drivers/%.c,$(SYSTEM)/$(ARCH)/drivers/%.mod,$(DRIVER_SRCS))

# Programs: user/bin/*.c become /system/<arch>/bin/* (command-line programs
# and the window server), user/apps/*.c /system/<arch>/apps/* (windowed apps,
# e.g. term), user/test/*.c /system/<arch>/test/* (test programs for
# scripts/test.sh).
# The library is an archive: a program gets only the parts it uses (the
# cryptography for SSH would otherwise be in every one). The start code is
# linked always.
# The same glyph tables as the kernel, for the window library (wfont.h).
USER_FONT_C   := $(GEN)/user_font_data.c
USER_LIB_SRCS := $(wildcard user/lib/*.c) common/crc32.c common/aes.c common/ip6text.c $(USER_FONT_C) user/arch/$(ARCH)/syscall.S \
                 user/arch/$(ARCH)/setjmp.S $(wildcard user/arch/$(ARCH)/string.S)
USER_LIB_OBJS := $(patsubst %,$(OUT)/user/%.o,$(USER_LIB_SRCS))
USER_START    := $(OUT)/user/user/arch/$(ARCH)/start.S.o
USER_LIB      := $(OUT)/user/libritteros.a
# The same position-independent (22l): the static-only parts of the
# position-independent programs.
USER_LIB_PIC  := $(OUT)/user/libritteros-pic.a
USER_HEADERS  := $(wildcard user/include/*.h user/include/sys/*.h user/include/netinet/*.h \
                            user/include/arpa/*.h) common/syscalls.h common/vmlayout.h common/elf.h common/aes.h common/ip6text.h common/settings_defaults.h \
                 user/lib/wfont_tables.h $(wildcard user/test/*.h)
# (42b) A windowed app is a bundle, /system/<arch>/apps/NAME.program/: its
# program NAME (user/apps/NAME.c), its file `info` (user/apps/NAME.info:
# name, about, the endings it can open) and its `icon.png` (user/apps/
# NAME.png, drawn by scripts/mkicons.py).
APP_NAMES     := $(patsubst user/apps/%.c,%,$(wildcard user/apps/*.c))
APP_PROGRAMS  := $(foreach a,$(APP_NAMES),$(SYSTEM)/$(ARCH)/apps/$(a).program/$(a))
USER_PROGRAMS := $(patsubst user/%.c,$(SYSTEM)/$(ARCH)/%,$(wildcard user/bin/*.c user/test/*.c)) $(APP_PROGRAMS)
APP_INFOS     := $(patsubst user/apps/%.info,$(SYSTEM)/$(ARCH)/apps/%.program/info,$(wildcard user/apps/*.info)) \
                 $(patsubst user/apps/%.png,$(SYSTEM)/$(ARCH)/apps/%.program/icon.png,$(wildcard user/apps/*.png))
# (51d) [ is test under another name (test looks at its argv[0]); the
# name is in a variable, so make does not take the bracket for a pattern.
BRACKET := $(SYSTEM)/$(ARCH)/bin/[

# Shared libraries (step 22c), in /system/<arch>/lib with the dynamic loader
# ld.so (22b): the library's own sources built position-independent, one
# library per part so each can be updated on its own. Each exports what its
# headers declare (scripts/libexports.c). The system's own SSH, cipher,
# GPT and mkfs code stays static, in libritteros.a only.
LIB_DIR_SYS   := $(SYSTEM)/$(ARCH)/lib
# (60) what cc needs beside the shared libraries: the start code and the
# static library (their rules are with cc's, below)
CC_LIB_FILES  := $(LIB_DIR_SYS)/start.o $(LIB_DIR_SYS)/libritteros.a
LDSO          := $(LIB_DIR_SYS)/ld.so
LDSO_OBJS     := $(OUT)/pic/user/arch/$(ARCH)/ldso.S.o $(OUT)/pic/user/ldso/ldso.c.o
LDTEST_STATIC := $(SYSTEM)/$(ARCH)/test/ldtest-static
TLSTEST_STATIC := $(SYSTEM)/$(ARCH)/test/tlstest-static
DLTEST_STATIC := $(SYSTEM)/$(ARCH)/test/dltest-static
# dlopen's test library (22k): with the test programs, not the system's libraries.
PLUG_LIB      := $(SYSTEM)/$(ARCH)/test/libplug.so.1
# The tests' own library in two versions and a program built with the first
# (22d): not installed; the test puts them on its image.
SWAP_DIR      := $(OUT)/swap
SWAP_FILES    := $(SWAP_DIR)/libswap.so.1-one $(SWAP_DIR)/libswap.so.1-two $(SWAP_DIR)/swaptest
STATIC_ONLY_SRCS := $(addprefix user/lib/,ssh.c gpt.c mkfs.c mkfs_fat.c mkfs_exfat.c mkfs_ritterfs.c \
                                          sha1.c wpacrypto.c sae.c \
                                          sha512.c chacha.c c25519.c int128.c)
WINDOW_SRCS   := user/lib/window.c user/lib/wfont.c user/lib/wchoose.c user/lib/wfield.c user/lib/ttf.c \
                 $(USER_FONT_C)
IMAGE_SRCS    := user/lib/image.c user/lib/inflate.c user/lib/deflate.c user/lib/png.c user/lib/jpeg.c
AUDIO_SRCS    := user/lib/audio.c
VIDEO_SRCS    := user/lib/video.c
DRAW_SRCS     := user/lib/draw.c
WIDGET_SRCS   := user/lib/widget.c
# TLS (item 23): libtls.so.1's own (its crypto: AES-GCM, SHA-384/HKDF, RSA and
# ECDSA checks, X.509), not libritteros's.
TLS_SRCS      := common/aes.c $(addprefix user/lib/,hkdf.c verify.c x509.c tls13.c tls.c)
# (with its own copies of SSH's SHA-512, ChaCha20-Poly1305 and X25519, which
# libritteros does not have: static-only)
LIBTLS_SRCS   := $(TLS_SRCS) $(addprefix user/lib/,sha512.c chacha.c c25519.c)
RITTEROS_SRCS := $(filter-out $(STATIC_ONLY_SRCS) $(WINDOW_SRCS) $(IMAGE_SRCS) $(AUDIO_SRCS) \
                              $(VIDEO_SRCS) $(DRAW_SRCS) $(WIDGET_SRCS) $(TLS_SRCS),$(USER_LIB_SRCS))
SHARED_LIBS   := $(addprefix $(LIB_DIR_SYS)/,libritteros.so.1 libwindow.so.1 libimage.so.1 \
                                             libaudio.so.1 libvideo.so.1 libdraw.so.1 libwidget.so.1 libtls.so.1)
pic_objs       = $(patsubst %,$(OUT)/pic/%.o,$(1))
USER_PIC_CFLAGS := $(filter-out -fno-pic -fno-pie,$(USER_CFLAGS)) -fPIC
SO_LDFLAGS    := -shared -z max-page-size=4096 -z now -z relro -z defs --hash-style=sysv
# Programs: dynamically linked (the loader binds everything at the start: -z
# now; libraries export functions only: -z nocopyreloc); --as-needed records
# only the libraries a program uses. libritteros-pic.a last, for the
# static-only parts. Position-independent since 22l (-pie, the objects of
# $(OUT)/pic/): the kernel puts each at a random place.
USER_DYN_LDFLAGS := -T user/user.ld -nostdlib -z max-page-size=4096 -pie \
                    -dynamic-linker /system/$(ARCH)/lib/ld.so -z now -z relro -z nocopyreloc \
                    --hash-style=sysv

# An architecture has a kernel once it has its entry code.
HAS_KERNEL  := $(wildcard $(ARCH_DIR)/start.S)

.PHONY: all arch-build disk run run-headless test clean distclean toolchain sdk sdk-arch FORCE

# Every build covers all architectures, so that shared code always compiles
# for each of them.
all:
	@for a in $(ARCHES); do $(MAKE) --no-print-directory ARCH=$$a arch-build || exit 1; done
	@$(MAKE) --no-print-directory disk CMDLINE="$(CMDLINE)"

# What a program built outside the tree links (sdk/ritteros.mk runs this):
# the programs' start code, the static library and the shared ones (22d),
# for every architecture; no kernel, no disk.
sdk:
	@for a in $(ARCHES); do $(MAKE) --no-print-directory ARCH=$$a sdk-arch || exit 1; done

sdk-arch: $(USER_START) $(USER_LIB) $(USER_LIB_PIC) $(SHARED_LIBS) $(LDSO)
	@:

# The bootloader, and the kernel if the architecture has one yet (otherwise
# at least the shared kernel code is compiled).
arch-build: $(BOOT_EFI) $(if $(HAS_KERNEL),$(KERNEL) $(MODULES),$(patsubst %,$(OUT)/kernel/%.o,$(SHARED_KERNEL_SRCS))) \
            $(LDSO) $(SHARED_LIBS) $(USER_PROGRAMS) $(APP_INFOS) $(BRACKET) $(LDTEST_STATIC) $(TLSTEST_STATIC) $(DLTEST_STATIC) $(PLUG_LIB) $(SWAP_FILES) \
            $(CC_LIB_FILES)

# --- bootloader ------------------------------------------------------------------

# (62g) With TOOLCHAIN=ritteros our cc compiles it as ELF objects (only
# efi_main and the firmware's functions in the Microsoft convention:
# EFIAPI) and our ld writes the PE32+ image (ld --efi).
ifeq ($(TOOLCHAIN),ritteros)
BOOT_CC   = $(USER_CC) -target $($(ARCH)_KERNEL_TARGET) -fno-pic $($(ARCH)_EFI_FLAGS) \
            $(subst $(ZIG_LIB)/include,$(USER_SYSTEM_INC),$(COMMON_CFLAGS)) -Iboot
BOOT_LINK = $(USER_LD) --efi -o $@ $(BOOT_OBJS)
else
BOOT_CC   = $(ZIG) clang $(EFI_CFLAGS)
BOOT_LINK = $(ZIG) lld-link $(EFI_LDFLAGS) -out:$@ $(BOOT_OBJS)
endif

$(OUT)/boot/%.c.obj: %.c $(HEADERS) $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(BOOT_CC) -c $< -o $@

$(BOOT_EFI): $(BOOT_OBJS) $(USER_TOOLS)
	@mkdir -p $(dir $@)
	$(BOOT_LINK)

# --- kernel ----------------------------------------------------------------------

# Rewritten only when the release or the commit changes (kernel/release.c
# includes it; nothing else is rebuilt).
$(VERSION_H): FORCE
	@mkdir -p $(dir $@)
	@printf '#define RITTEROS_VERSION "%s"\n#define RITTEROS_COMMIT "%s"\n' "$(VERSION_TEXT)" "$(VERSION_COMMIT)" > $@.new
	@if [ -f $@ ] && [ "$$(cat $@)" = "$$(cat $@.new)" ]; then rm $@.new; else mv $@.new $@; fi

$(OUT)/kernel/kernel/release.c.o: $(VERSION_H)

$(FONT_C): fonts/9x15.bdf fonts/9x15B.bdf $(HOST_TOOLS)/bdf2c
	@mkdir -p $(dir $@)
	$(HOST_TOOLS)/bdf2c $@ $(FONTS)

$(USER_FONT_C): fonts/9x15.bdf fonts/9x15B.bdf $(HOST_TOOLS)/bdf2c user/lib/wfont_tables.h
	@mkdir -p $(dir $@)
	$(HOST_TOOLS)/bdf2c --include=../../user/lib/wfont_tables.h $@ $(FONTS)

$(OUT)/kernel/%.c.o: %.c $(HEADERS) $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) $(KERNEL_CFLAGS) -c $< -o $@

$(OUT)/kernel/%.S.o: %.S $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) -target $($(ARCH)_KERNEL_TARGET) -c $< -o $@

$(EXPORTS_C): $(DRIVER_HEADERS) $(HOST_TOOLS)/kexports
	@mkdir -p $(dir $@)
	$(HOST_TOOLS)/kexports table $@ $(DRIVER_HEADERS)

# A module may use only what the driver interface declares (kexports check).
$(SYSTEM)/$(ARCH)/drivers/%.mod: drivers/%.c $(DRIVER_HEADERS) $(DRIVER_PARTS) $(HOST_TOOLS)/kexports $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) $(MODULE_CFLAGS) -c $< -o $@.o
	$(HOST_TOOLS)/kexports check $@.o $(DRIVER_HEADERS) || { rm -f $@.o; exit 1; }
	mv $@.o $@

$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld $(USER_TOOLS)
	@mkdir -p $(dir $@)
	$(USER_LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

# --- RitterOS's own toolchain for this machine (62d) ----------------------------

$(TOOLCHAIN_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(BUILD)/toolchain-*
	@touch $@

$(HOST_TOOLS)/cc: user/bin/cc.c $(wildcard user/bin/cc/*.c user/bin/cc/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Iuser/bin -o $@ user/bin/cc.c $(wildcard user/bin/cc/*.c)

$(HOST_TOOLS)/as: user/bin/as.c $(wildcard user/bin/as/*.c user/bin/as/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -Iuser/bin -o $@ user/bin/as.c $(wildcard user/bin/as/*.c)

$(HOST_TOOLS)/ld: user/bin/ld.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -o $@ user/bin/ld.c

$(HOST_TOOLS)/ar: user/bin/ar.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -o $@ user/bin/ar.c

$(GENERATORS): $(HOST_TOOLS)/%: scripts/%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 -O2 -o $@ $<

# --- programs --------------------------------------------------------------------

$(OUT)/user/%.c.o: %.c $(USER_HEADERS) $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c $< -o $@

$(OUT)/user/%.S.o: %.S $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) -target $($(ARCH)_KERNEL_TARGET) -c $< -o $@

# The exFAT up-case table that format -t exfat writes (user/lib/mkfs_exfat.c)
# (scripts/exfat_upcase.c: the capitals of the Unicode Character Database).
UPCASE_H := $(GEN)/exfat_upcase.h

$(UPCASE_H): $(HOST_TOOLS)/exfat_upcase
	@mkdir -p $(dir $@)
	$(HOST_TOOLS)/exfat_upcase $@

# math.c: sqrt as the processor's instruction (without errno, which it never
# sets); its double-double arithmetic needs a*b+c rounded twice, never fused
# (ARM's compiler would make one FMA of it: -ffp-contract=off); its constants
# come from scripts/mathconst.c (22s; in C since 63).
LIBM_H := $(GEN)/libm_tables.h
$(LIBM_H): $(HOST_TOOLS)/mathconst
	@mkdir -p $(dir $@)
	$(HOST_TOOLS)/mathconst $@
$(OUT)/user/user/lib/math.c.o: $(LIBM_H)
$(OUT)/pic/user/lib/math.c.o: $(LIBM_H)
$(OUT)/user/user/lib/math.c.o: USER_CFLAGS += -fno-math-errno -ffp-contract=off -I$(GEN)
$(OUT)/pic/user/lib/math.c.o: USER_PIC_CFLAGS += -fno-math-errno -ffp-contract=off -I$(GEN)
# complex.c: exact products (Dekker) in clog near |z| = 1, as math.c
$(OUT)/user/user/lib/complex.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/lib/complex.c.o: USER_PIC_CFLAGS += -ffp-contract=off
# (41b) libdraw's pixels the same on every machine: no fused multiply-adds.
$(OUT)/user/user/lib/draw.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/lib/draw.c.o: USER_PIC_CFLAGS += -ffp-contract=off
$(OUT)/user/user/lib/widget.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/lib/widget.c.o: USER_PIC_CFLAGS += -ffp-contract=off
# (41c-3) the font reader's glyph masks too, wfont's pen, and ttftest, which builds it in.
$(OUT)/user/user/lib/wfont.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/lib/wfont.c.o: USER_PIC_CFLAGS += -ffp-contract=off
$(OUT)/user/user/lib/ttf.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/lib/ttf.c.o: USER_PIC_CFLAGS += -ffp-contract=off
$(OUT)/user/user/test/ttftest.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/test/ttftest.c.o: USER_PIC_CFLAGS += -ffp-contract=off
$(OUT)/user/user/test/fonttest.c.o: USER_CFLAGS += -ffp-contract=off
$(OUT)/pic/user/test/fonttest.c.o: USER_PIC_CFLAGS += -ffp-contract=off
# mathtest calls the library, never the compiler's own evaluation of a call
# with known arguments, and makes its arguments without fused multiply-adds
# (ARM's compiler fuses a + b * c otherwise): the test compares it with the
# Mac bit for bit.
$(OUT)/pic/user/test/mathtest.c.o: USER_PIC_CFLAGS += -fno-builtin -ffp-contract=off
$(OUT)/pic/user/test/stringtest.c.o: USER_PIC_CFLAGS += -fno-builtin

$(OUT)/user/user/lib/mkfs_exfat.c.o: $(UPCASE_H)
$(OUT)/user/user/lib/mkfs_exfat.c.o: USER_CFLAGS += -I$(GEN)

$(USER_LIB): $(USER_LIB_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(USER_AR) rcs $@ $(filter %.o,$^)

$(USER_LIB_PIC): $(call pic_objs,$(USER_LIB_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@
	$(USER_AR) rcs $@ $(filter %.o,$^)

$(OUT)/pic/user/lib/mkfs_exfat.c.o: $(UPCASE_H)
$(OUT)/pic/user/lib/mkfs_exfat.c.o: USER_PIC_CFLAGS += -I$(GEN)

$(SYSTEM)/$(ARCH)/%: $(OUT)/pic/user/%.c.o $(USER_START) $(USER_LIB_PIC) $(SHARED_LIBS) $(LDSO) user/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) $(USER_DYN_LDFLAGS) -o $@ $(USER_START) $< $(PARTS) --as-needed $(SHARED_LIBS) \
	    --no-as-needed $(USER_LIB_PIC)

# (42b) An app's program, inside its bundle.
define app_program
$(SYSTEM)/$(ARCH)/apps/$(1).program/$(1): $(OUT)/pic/user/apps/$(1).c.o $(USER_START) $(USER_LIB_PIC) $(SHARED_LIBS) $(LDSO) user/user.ld
	@mkdir -p $$(dir $$@)
	$(USER_LD) $(USER_DYN_LDFLAGS) -o $$@ $(USER_START) $$< $$(PARTS) --as-needed $(SHARED_LIBS) \
	    --no-as-needed $(USER_LIB_PIC)
endef
$(foreach a,$(APP_NAMES),$(eval $(call app_program,$(a))))

$(BRACKET): $(SYSTEM)/$(ARCH)/bin/test
	cp $< '$@'

# JavaScript (27o): the engine in user/lib/js/ (a folder: the library's
# wildcard misses it), linked into each program that runs scripts.
JS_PARTS := $(call pic_objs,$(wildcard user/lib/js/*.c))
$(SYSTEM)/$(ARCH)/bin/js: $(JS_PARTS)
$(SYSTEM)/$(ARCH)/bin/js: PARTS = $(JS_PARTS)
$(JS_PARTS) $(OUT)/pic/user/bin/js.c.o: $(wildcard user/lib/js/*.h user/lib/js/*.def)

# A program of several files (iBrowser, 27a): user/apps/NAME.c and its parts
# in user/apps/NAME/ (a folder, so the wildcard of programs misses them).
IBROWSER_PARTS := $(call pic_objs,$(wildcard user/apps/ibrowser/*.c))
# (27b) gzip pages: libimage's inflater, linked in (the library does not export it)
IBROWSER_INFLATE := $(call pic_objs,user/lib/inflate.c)
$(SYSTEM)/$(ARCH)/apps/ibrowser.program/ibrowser: $(IBROWSER_PARTS) $(IBROWSER_INFLATE) $(JS_PARTS)
$(SYSTEM)/$(ARCH)/apps/ibrowser.program/ibrowser: PARTS = $(IBROWSER_PARTS) $(IBROWSER_INFLATE) $(JS_PARTS)
$(IBROWSER_PARTS) $(OUT)/pic/user/apps/ibrowser.c.o: user/apps/ibrowser/browser.h
$(OUT)/pic/user/apps/ibrowser/dom.c.o: $(wildcard user/lib/js/*.h user/lib/js/*.def)

# The C compiler (60): cc, as and ld, programs of user/bin; cc's and as's
# parts in folders of their own (user/bin/cc/, user/bin/as/).
CC_PARTS := $(call pic_objs,$(wildcard user/bin/cc/*.c))
$(SYSTEM)/$(ARCH)/bin/cc: $(CC_PARTS)
$(SYSTEM)/$(ARCH)/bin/cc: PARTS = $(CC_PARTS)
$(CC_PARTS) $(OUT)/pic/user/bin/cc.c.o: $(wildcard user/bin/cc/*.h)
AS_PARTS := $(call pic_objs,$(wildcard user/bin/as/*.c))
$(SYSTEM)/$(ARCH)/bin/as: $(AS_PARTS)
$(SYSTEM)/$(ARCH)/bin/as: PARTS = $(AS_PARTS)
$(AS_PARTS) $(OUT)/pic/user/bin/as.c.o: user/bin/as/as.h

# What cc needs on the disk (60): per architecture the start code and the
# static library beside the shared ones in /system/<arch>/lib (the headers,
# one copy for both, are SYSTEM_INCLUDE below).
$(LIB_DIR_SYS)/start.o: $(USER_START)
	@mkdir -p $(dir $@)
	cp $< $@
$(LIB_DIR_SYS)/libritteros.a: $(USER_LIB_PIC)
	@mkdir -p $(dir $@)
	cp $< $@

# The library sources again, position-independent, for the shared libraries.
$(OUT)/pic/%.c.o: %.c $(USER_HEADERS) $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_PIC_CFLAGS) -c $< -o $@

$(OUT)/pic/%.S.o: %.S $(TOOLCHAIN_STAMP) $(USER_TOOLS) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) -target $($(ARCH)_KERNEL_TARGET) -fPIC -c $< -o $@

# A library's version script: the functions its headers declare.
$(GEN)/%.map: $(HOST_TOOLS)/libexports $(USER_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_TOOLS)/libexports $* $@

# A shared library: $(1) its file name (also its soname), $(2) its sources,
# $(3) the libraries it needs.
define shared_lib
$(LIB_DIR_SYS)/$(1): $(call pic_objs,$(2)) $(GEN)/$(1).map $(addprefix $(LIB_DIR_SYS)/,$(3))
	@mkdir -p $$(dir $$@)
	$(USER_LD) $(SO_LDFLAGS) -soname $(1) --version-script=$(GEN)/$(1).map -o $$@ \
	    $(call pic_objs,$(2)) $(addprefix $(LIB_DIR_SYS)/,$(3))
endef
$(eval $(call shared_lib,libritteros.so.1,$(RITTEROS_SRCS),))
$(eval $(call shared_lib,libwindow.so.1,$(WINDOW_SRCS),libritteros.so.1))
$(eval $(call shared_lib,libimage.so.1,$(IMAGE_SRCS),libritteros.so.1))
$(eval $(call shared_lib,libaudio.so.1,$(AUDIO_SRCS),libritteros.so.1))
$(eval $(call shared_lib,libvideo.so.1,$(VIDEO_SRCS),libritteros.so.1))
$(eval $(call shared_lib,libdraw.so.1,$(DRAW_SRCS),libritteros.so.1))
$(eval $(call shared_lib,libwidget.so.1,$(WIDGET_SRCS),libwindow.so.1 libdraw.so.1 libritteros.so.1))
$(eval $(call shared_lib,libtls.so.1,$(LIBTLS_SRCS),libritteros.so.1))

# ldtest built statically too (22c): a program linked without the shared
# libraries still runs (the test ldso compares the two).
$(LDTEST_STATIC): $(OUT)/user/user/test/ldtest.c.o $(USER_START) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_START) $< $(USER_LIB)

# dltest statically too (22k: dlopen fails there), and its library.
$(DLTEST_STATIC): $(OUT)/user/user/test/dltest.c.o $(USER_START) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_START) $< $(USER_LIB)

$(PLUG_LIB): $(OUT)/pic/user/test/plug/plug.c.o $(LIB_DIR_SYS)/libritteros.so.1
	@mkdir -p $(dir $@)
	$(USER_LD) $(SO_LDFLAGS) -soname libplug.so.1 -o $@ $< $(LIB_DIR_SYS)/libritteros.so.1

# tlstest built statically too (22i): per-thread data from the start code.
$(TLSTEST_STATIC): $(OUT)/user/user/test/tlstest.c.o $(USER_START) $(USER_LIB) user/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) $(USER_LDFLAGS) -o $@ $(USER_START) $< $(USER_LIB)

$(SWAP_DIR)/libswap.so.1-%: $(OUT)/pic/user/test/libswap/swap_%.c.o
	@mkdir -p $(dir $@)
	$(USER_LD) $(SO_LDFLAGS) -soname libswap.so.1 -o $@ $<

$(SWAP_DIR)/swaptest: $(OUT)/pic/user/test/libswap/swaptest.c.o $(SWAP_DIR)/libswap.so.1-one \
                      $(USER_START) $(USER_LIB_PIC) $(SHARED_LIBS) user/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) $(USER_DYN_LDFLAGS) -o $@ $(USER_START) $< --as-needed $(SHARED_LIBS) \
	    $(SWAP_DIR)/libswap.so.1-one --no-as-needed $(USER_LIB_PIC)

# The dynamic loader: a static, position-independent program (user/ldso.ld; 22l).
$(LDSO): $(LDSO_OBJS) $(USER_LIB_PIC) user/ldso.ld
	@mkdir -p $(dir $@)
	$(USER_LD) -T user/ldso.ld -nostdlib -pie --no-dynamic-linker -Bstatic -z max-page-size=4096 \
	    -z norelro -o $@ $(LDSO_OBJS) $(USER_LIB_PIC)

# --- disk image ------------------------------------------------------------------

# Rewritten only when CMDLINE changes, so the disk is not rebuilt needlessly.
$(CMDLINE_FILE): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' "$(CMDLINE)" > $@.new
	@if cmp -s $@.new $@; then rm $@.new; else mv $@.new $@; echo "cmdline: '$(CMDLINE)'"; fi

# The fonts (22c): every BDF of fonts/ as /system/fonts/NAME.bdf, one copy for
# both architectures (libwindow reads them; 9x15 is also compiled in).
# (27k) Adobe's faces bring their licence along (fonts/SOURCE.txt).
# (41c) GNU FreeFont's TrueType and (41c-2) CFF faces with their README and GPL.
SYSTEM_FONTS := $(patsubst fonts/%,$(SYSTEM)/fonts/%,$(wildcard fonts/*.bdf fonts/*.ttf fonts/*.otf)) \
                $(SYSTEM)/fonts/ADOBE-COPYING.txt $(SYSTEM)/fonts/FreeFont-README.txt \
                $(SYSTEM)/fonts/FreeFont-COPYING.txt
$(SYSTEM)/fonts/%.bdf: fonts/%.bdf
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/fonts/%.ttf: fonts/%.ttf
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/fonts/%.otf: fonts/%.otf
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/fonts/%.txt: fonts/%.txt
	@mkdir -p $(dir $@)
	cp $< $@

# The root certificates libtls trusts (23e): Mozilla's, certs/SOURCE.txt.
SYSTEM_CERTS := $(SYSTEM)/certs/cacert.pem
$(SYSTEM)/certs/%: certs/%
	@mkdir -p $(dir $@)
	cp $< $@

# Wi-Fi firmware (24f): MediaTek's for the MT7921, (36a) Intel's for the
# 8265; firmware/SOURCE.txt.
SYSTEM_FIRMWARE := $(patsubst firmware/%,$(SYSTEM)/firmware/%,$(wildcard firmware/*.bin firmware/*.ucode))
$(SYSTEM)/firmware/%: firmware/%
	@mkdir -p $(dir $@)
	cp $< $@

# The C headers for cc on RitterOS (60): user/include and cc's own
# (user/bin/cc/include: stddef.h, stdint.h, stdarg.h, ...) as
# /system/include, one copy for both architectures. The tree's headers
# reach common/syscalls.h by a relative path; on the disk it is
# include/ritteros/syscalls.h.
SYSTEM_INCLUDE := $(patsubst user/include/%,$(SYSTEM)/include/%,$(wildcard user/include/*.h user/include/*/*.h)) \
                  $(patsubst user/bin/cc/include/%,$(SYSTEM)/include/%,$(wildcard user/bin/cc/include/*.h)) \
                  $(SYSTEM)/include/ritteros/syscalls.h \
                  $(patsubst drivers/include/%,$(SYSTEM)/include/drivers/%,$(DRIVER_HEADERS))
# (67b) The driver interface, for drivers built on the machine.
$(SYSTEM)/include/drivers/%.h: drivers/include/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/include/ritteros/syscalls.h: common/syscalls.h
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/include/%.h: user/include/%.h
	@mkdir -p $(dir $@)
	sed 's|#include "\(\.\./\)*common/syscalls.h"|#include <ritteros/syscalls.h>|' $< > $@
$(SYSTEM)/include/%.h: user/bin/cc/include/%.h
	@mkdir -p $(dir $@)
	cp $< $@

# The SDK on the machine (63f): /system/sdk, the build fragment that builds a
# project there with the machine's own cc, and its guide and API list.
SYSTEM_SDK := $(SYSTEM)/sdk/ritteros.mk $(SYSTEM)/sdk/CLAUDE.md $(SYSTEM)/sdk/API.md
$(SYSTEM)/sdk/%: sdk/%
	@mkdir -p $(dir $@)
	cp $< $@

# The manual on the machine (50j): docs/ as /system/doc, a page per program
# (docs/commands/), the install guide and the user's manual, read by `man`.
SYSTEM_DOC := $(patsubst docs/commands/%,$(SYSTEM)/doc/commands/%,$(wildcard docs/commands/*.md)) \
              $(SYSTEM)/doc/install.md $(SYSTEM)/doc/manual.md
$(SYSTEM)/doc/commands/%.md: docs/commands/%.md
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/$(ARCH)/apps/%.program/info: user/apps/%.info
	@mkdir -p $(dir $@)
	cp $< $@

$(SYSTEM)/$(ARCH)/apps/%.program/icon.png: user/apps/%.png
	@mkdir -p $(dir $@)
	cp $< $@

$(SYSTEM)/doc/%.md: docs/%.md
	@mkdir -p $(dir $@)
	cp $< $@

disk: $(SYSTEM_FONTS) $(SYSTEM_CERTS) $(SYSTEM_FIRMWARE) $(SYSTEM_INCLUDE) $(SYSTEM_SDK) $(SYSTEM_DOC) $(DISKS)

# Rewrites the partition tables and the ESP; partition 2 keeps its contents,
# and changed files are installed into it with a RitterFS commit. Each disk
# holds one architecture (55a).
SHARED_SYSTEM := $(wildcard $(SYSTEM)/fonts/* $(SYSTEM)/certs/* $(SYSTEM)/firmware/* $(SYSTEM)/sdk/*)
.SECONDEXPANSION:
$(BUILD)/ritteros-%.img: $$(wildcard $(ESP)/EFI/BOOT/*.EFI $(SYSTEM)/$$*/kernel.elf $(SYSTEM)/$$*/bin/* \
                         $(SYSTEM)/$$*/apps/*/* $(SYSTEM)/$$*/test/* $(SYSTEM)/$$*/lib/*) \
                         $(SHARED_SYSTEM) $(CMDLINE_FILE) scripts/mkdisk.py scripts/ritterfs.py
	python3 scripts/mkdisk.py --arch $* $@ $(ESP) $(SYSTEM)

# --- running ---------------------------------------------------------------------

# UEFI variable store; the firmware writes to it, so each build gets its own copy.
$(BUILD)/%/vars.fd:
	@mkdir -p $(dir $@)
	cp $($*_FW_VARS) $@

# Graphical window; this terminal is the serial port (the RitterOS terminal).
# Ctrl-C goes to RitterOS; Ctrl-A X quits QEMU, Ctrl-A C toggles its monitor.
SERIAL_STDIO := -chardev stdio,id=serial0,mux=on,signal=off -serial chardev:serial0 \
                -mon chardev=serial0,mode=readline

# A network card (Intel e1000) on QEMU's "user" network: its DHCP server
# and DNS forwarder, and the Mac's own connection to the internet (the test
# boots add the card themselves where they need it).
NETWORK := -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device e1000,netdev=net0

run: all $(OUT)/vars.fd
	$(call qemu_cmd,$(ARCH)) -device usb-tablet,bus=xhci.0,port=2 $(NETWORK) $(SERIAL_STDIO)

# No window; console on this terminal (Ctrl-A X quits QEMU).
run-headless: all $(OUT)/vars.fd
	$(call qemu_cmd,$(ARCH)) $(NETWORK) -nographic

# The normal test boots use an empty command line, whatever CMDLINE says.
# TEST_ARCH=x86 or arm runs one half; both (the default) side by side.
TEST_ARCH ?= both
test: override CMDLINE :=
test: all $(foreach a,$(ARCHES),$(BUILD)/$(a)/vars.fd)
	TEST_ARCH=$(TEST_ARCH) scripts/test.sh "$(call qemu_cmd,x86_64)" "$(call qemu_cmd,aarch64)" \
	    $(BUILD)/ritteros-x86_64.img $(BUILD)/ritteros-aarch64.img

toolchain $(ZIG):
	scripts/setup-toolchain.sh

clean:
	rm -rf $(BUILD)

# Also removes the toolchain (re-run `make toolchain` afterwards).
distclean: clean
	rm -rf tools
