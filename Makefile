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
# (68k) Its debug information names the checkout as `.`, never the folder
# it was built in: a release's kernel is the same bytes wherever 2026.11 is
# built (our cc ignores the flag; it writes no debug information).
KERNEL_CFLAGS  := -target $($(ARCH)_KERNEL_TARGET) -fno-pic -fno-pie -mgeneral-regs-only \
                  $($(ARCH)_KERNEL_FLAGS) -g -ffile-prefix-map=$(CURDIR)=. $(subst $(ZIG_LIB)/include,$(USER_SYSTEM_INC),$(COMMON_CFLAGS)) -Ikernel \
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
                            user/include/arpa/*.h user/include/net/*.h) common/syscalls.h common/vmlayout.h common/elf.h common/aes.h common/ip6text.h common/settings_defaults.h \
                 user/lib/wfont_tables.h $(wildcard user/test/*.h)
# (42b) A windowed app is a bundle, /system/<arch>/apps/NAME.program/: its
# program NAME (user/apps/NAME.c), its file `info` (user/apps/NAME.info:
# name, about, the endings it can open) and its `icon.png` (user/apps/
# NAME.png, drawn by scripts/mkicons.py).
APP_NAMES     := $(patsubst user/apps/%.c,%,$(wildcard user/apps/*.c))
APP_PROGRAMS  := $(foreach a,$(APP_NAMES),$(SYSTEM)/$(ARCH)/apps/$(a).program/$(a))
USER_PROGRAMS := $(patsubst user/%.c,$(SYSTEM)/$(ARCH)/%,$(wildcard user/bin/*.c user/test/*.c)) $(APP_PROGRAMS)
# (46) Test programs of the system libraries from upstream (below): none
# with TOOLCHAIN=ritteros, which does not build those libraries.
UPSTREAM_TESTS := $(addprefix $(SYSTEM)/$(ARCH)/test/,zlibtest openssltest netlibstest curltest)
ifeq ($(TOOLCHAIN),ritteros)
USER_PROGRAMS := $(filter-out $(UPSTREAM_TESTS),$(USER_PROGRAMS))
endif
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
# (70b) libkeyring.so.1: a program's requests to its user's keyringd.
KEYRING_SRCS  := user/lib/keyring.c
# TLS (item 23): libtls.so.1's own (its crypto: AES-GCM, SHA-384/HKDF, RSA and
# ECDSA checks, X.509), not libritteros's.
TLS_SRCS      := common/aes.c $(addprefix user/lib/,hkdf.c verify.c x509.c tls13.c tls.c)
# (with its own copies of SSH's SHA-512, ChaCha20-Poly1305 and X25519, which
# libritteros does not have: static-only)
LIBTLS_SRCS   := $(TLS_SRCS) $(addprefix user/lib/,sha512.c chacha.c c25519.c)
RITTEROS_SRCS := $(filter-out $(STATIC_ONLY_SRCS) $(WINDOW_SRCS) $(IMAGE_SRCS) $(AUDIO_SRCS) \
                              $(VIDEO_SRCS) $(DRAW_SRCS) $(WIDGET_SRCS) $(KEYRING_SRCS) $(TLS_SRCS),$(USER_LIB_SRCS))
SHARED_LIBS   := $(addprefix $(LIB_DIR_SYS)/,libritteros.so.1 libwindow.so.1 libimage.so.1 \
                                             libaudio.so.1 libvideo.so.1 libdraw.so.1 libwidget.so.1 libtls.so.1 \
                                             libkeyring.so.1)
pic_objs       = $(patsubst %,$(OUT)/pic/%.o,$(1))
# (43c) The C++ libraries and programs (below: not with TOOLCHAIN=ritteros).
ifneq ($(TOOLCHAIN),ritteros)
CXX_LIBS      := $(addprefix $(LIB_DIR_SYS)/,libunwind.so.1 libc++abi.so.1 libc++.so.1)
CXX_PROGRAMS  := $(SYSTEM)/$(ARCH)/test/cxxtest
# (46) The system libraries from upstream (their section, below).
UPSTREAM_LIBS := $(addprefix $(LIB_DIR_SYS)/,libz.so.1 libcrypto.so.3 libssl.so.3 libnghttp2.so.14 \
                                            libbrotlicommon.so.1 libbrotlidec.so.1 libbrotlienc.so.1 libpsl.so.5 \
                                            libcurl.so.4)
UPSTREAM_PROGRAMS := $(SYSTEM)/$(ARCH)/bin/openssl $(SYSTEM)/$(ARCH)/bin/curl
endif
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

sdk-arch: $(USER_START) $(USER_LIB) $(USER_LIB_PIC) $(SHARED_LIBS) $(CXX_LIBS) $(UPSTREAM_LIBS) $(LDSO)
	@:

# The bootloader, and the kernel if the architecture has one yet (otherwise
# at least the shared kernel code is compiled).
arch-build: $(BOOT_EFI) $(if $(HAS_KERNEL),$(KERNEL) $(MODULES),$(patsubst %,$(OUT)/kernel/%.o,$(SHARED_KERNEL_SRCS))) \
            $(LDSO) $(SHARED_LIBS) $(CXX_LIBS) $(UPSTREAM_LIBS) $(UPSTREAM_PROGRAMS) $(USER_PROGRAMS) $(CXX_PROGRAMS) $(APP_INFOS) $(BRACKET) $(LDTEST_STATIC) $(TLSTEST_STATIC) $(DLTEST_STATIC) $(PLUG_LIB) $(SWAP_FILES) \
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

# The C compiler (60): cc, as and ld, programs of user/bin; cc's and as's
# parts in folders of their own (user/bin/cc/, user/bin/as/).
CC_PARTS := $(call pic_objs,$(wildcard user/bin/cc/*.c))
$(SYSTEM)/$(ARCH)/bin/cc: $(CC_PARTS)
$(SYSTEM)/$(ARCH)/bin/cc: PARTS = $(CC_PARTS)
$(CC_PARTS) $(OUT)/pic/user/bin/cc.c.o: $(wildcard user/bin/cc/*.h)
# (70a, 70b) The keyring's file format and keyringd's messages, shared.
$(OUT)/pic/user/bin/keyring.c.o $(OUT)/pic/user/bin/keyringd.c.o $(OUT)/pic/user/bin/passwd.c.o \
    $(OUT)/pic/user/bin/login.c.o $(OUT)/pic/user/lib/keyring.c.o: user/bin/keyring_db.h user/lib/keyring_wire.h
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
$(eval $(call shared_lib,libkeyring.so.1,$(KEYRING_SRCS),libritteros.so.1))

# --- (43c) C++ ---------------------------------------------------------------------
# libunwind, libc++abi and libc++ from the pinned zig's LLVM sources (Apache-2.0
# with the LLVM exception, an agreed exception to the licence rule: README's
# "Licence"), shared libraries in /system/<arch>/lib, and C++ programs against
# them (cxxtest). Built with zig's clang and linked with its lld only: C++ is
# built on the Mac (with TOOLCHAIN=ritteros, our cc being C's, none of it is).
# libc++'s configuration is user/cxx/__config_site, given to every C++ compile
# (the pinned zig's libc++ takes it as flags, not from a header of its own).
# As LLVM's defaults: operator new and delete are libc++abi's (libc++'s
# new.cpp left out), exceptions on (cxa_noexception.cpp left out).
ifneq ($(TOOLCHAIN),ritteros)
CXX_CONFIG    := user/cxx/__config_site
CXX_COMMON    := -target $($(ARCH)_KERNEL_TARGET) -nostdinc -fPIC -O2 -fno-stack-protector -funwind-tables
CXX_C_INC     := -isystem user/include -isystem $(ZIG_LIB)/include
CXX_INC       := -nostdinc++ -include $(CXX_CONFIG) -isystem $(ZIG_LIB)/libcxx/include \
                 -isystem $(ZIG_LIB)/libcxxabi/include $(CXX_C_INC)
CXX_DEPS      := $(USER_HEADERS) $(CXX_CONFIG) $(TOOLCHAIN_STAMP) | $(ZIG_DEP)
UNWIND_SRCS   := libunwind.cpp UnwindLevel1.c UnwindLevel1-gcc-ext.c gcc_personality_v0.c \
                 UnwindRegistersRestore.S UnwindRegistersSave.S
CXXABI_SRCS   := $(filter-out cxa_noexception.cpp,$(notdir $(wildcard $(ZIG_LIB)/libcxxabi/src/*.cpp)))
LIBCXX_SRCS   := $(filter-out new.cpp,$(notdir $(wildcard $(ZIG_LIB)/libcxx/src/*.cpp))) \
                 $(patsubst $(ZIG_LIB)/libcxx/src/%,%,$(wildcard $(ZIG_LIB)/libcxx/src/filesystem/*.cpp \
                                                              $(ZIG_LIB)/libcxx/src/ryu/*.cpp))
UNWIND_OBJS   := $(patsubst %,$(OUT)/cxx/unwind/%.o,$(UNWIND_SRCS))
CXXABI_OBJS   := $(patsubst %,$(OUT)/cxx/abi/%.o,$(CXXABI_SRCS))
LIBCXX_OBJS   := $(patsubst %,$(OUT)/cxx/libcxx/%.o,$(LIBCXX_SRCS))
# (the shared libraries' helpers for __int128 from libritteros-pic.a, kept local)
CXX_SO_LDFLAGS := $(SO_LDFLAGS) --eh-frame-hdr --exclude-libs=ALL

$(OUT)/cxx/unwind/%.cpp.o: $(ZIG_LIB)/libunwind/src/%.cpp $(CXX_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(CXX_COMMON) -x c++ -std=c++17 -fno-exceptions -fno-rtti $(CXX_INC) \
	    -I$(ZIG_LIB)/libunwind/include -c $< -o $@
$(OUT)/cxx/unwind/%.c.o: $(ZIG_LIB)/libunwind/src/%.c $(CXX_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(CXX_COMMON) -std=c11 $(CXX_C_INC) -I$(ZIG_LIB)/libunwind/include -c $< -o $@
$(OUT)/cxx/unwind/%.S.o: $(ZIG_LIB)/libunwind/src/%.S $(CXX_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(CXX_COMMON) $(CXX_C_INC) -I$(ZIG_LIB)/libunwind/include -c $< -o $@
$(OUT)/cxx/abi/%.cpp.o: $(ZIG_LIB)/libcxxabi/src/%.cpp $(CXX_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(CXX_COMMON) -std=c++23 -D_LIBCXXABI_BUILDING_LIBRARY -D_LIBCPP_BUILDING_LIBRARY \
	    -DHAVE___CXA_THREAD_ATEXIT_IMPL -D_LIBCXXABI_USE_LLVM_UNWINDER $(CXX_INC) -I$(ZIG_LIB)/libcxx/src \
	    -I$(ZIG_LIB)/libunwind/include -c $< -o $@
# (_POSIX_TIMERS for chrono.cpp's and the file system clock's clock_gettime:
# the library has the clocks, not timer_create, so unistd.h does not claim it)
$(OUT)/cxx/libcxx/%.cpp.o: $(ZIG_LIB)/libcxx/src/%.cpp $(CXX_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(CXX_COMMON) -std=c++23 -D_LIBCPP_BUILDING_LIBRARY -DLIBCXX_BUILDING_LIBCXXABI \
	    -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES -D_POSIX_TIMERS=200809L $(CXX_INC) -I$(ZIG_LIB)/libcxx/src \
	    -I$(ZIG_LIB)/libcxx/libc -c $< -o $@

$(LIB_DIR_SYS)/libunwind.so.1: $(UNWIND_OBJS) $(LIB_DIR_SYS)/libritteros.so.1
	@mkdir -p $(dir $@)
	$(USER_LD) $(CXX_SO_LDFLAGS) -soname libunwind.so.1 -o $@ $(UNWIND_OBJS) $(LIB_DIR_SYS)/libritteros.so.1
$(LIB_DIR_SYS)/libc++abi.so.1: $(CXXABI_OBJS) $(LIB_DIR_SYS)/libunwind.so.1 $(LIB_DIR_SYS)/libritteros.so.1
	@mkdir -p $(dir $@)
	$(USER_LD) $(CXX_SO_LDFLAGS) -soname libc++abi.so.1 -o $@ $(CXXABI_OBJS) \
	    $(LIB_DIR_SYS)/libunwind.so.1 $(LIB_DIR_SYS)/libritteros.so.1
$(LIB_DIR_SYS)/libc++.so.1: $(LIBCXX_OBJS) $(LIB_DIR_SYS)/libc++abi.so.1 $(LIB_DIR_SYS)/libunwind.so.1 \
                            $(LIB_DIR_SYS)/libritteros.so.1 $(USER_LIB_PIC)
	@mkdir -p $(dir $@)
	$(USER_LD) $(CXX_SO_LDFLAGS) -soname libc++.so.1 -o $@ $(LIBCXX_OBJS) $(LIB_DIR_SYS)/libc++abi.so.1 \
	    $(LIB_DIR_SYS)/libunwind.so.1 $(LIB_DIR_SYS)/libritteros.so.1 $(USER_LIB_PIC)

# C++ programs (user/test/cxx/NAME.cpp): as C ones, with unwinding tables
# (user.ld keeps them) and the C++ libraries.
$(OUT)/cxx/prog/%.cpp.o: user/test/cxx/%.cpp $(CXX_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(CXX_COMMON) -std=c++23 -Wall -Wextra -Werror $(CXX_INC) -c $< -o $@
$(SYSTEM)/$(ARCH)/test/%: $(OUT)/cxx/prog/%.cpp.o $(USER_START) $(USER_LIB_PIC) $(CXX_LIBS) $(SHARED_LIBS) \
                          $(LDSO) user/user.ld
	@mkdir -p $(dir $@)
	$(USER_LD) $(USER_DYN_LDFLAGS) --eh-frame-hdr -o $@ $(USER_START) $< --as-needed $(CXX_LIBS) $(SHARED_LIBS) \
	    --no-as-needed $(USER_LIB_PIC)
endif

# --- (46) System libraries from upstream ---------------------------------------------
# Libraries RitterOS ships unchanged from their own sources, for ported
# programs and apps (the WebKit port first): their versions and archives'
# hashes are user/upstream/versions, their sources tools/src/NAME-VERSION
# (scripts/setup-toolchain.sh), their notes and licences user/upstream/
# (an agreed exception to the licence rule, item 46). Built with zig's
# clang and lld only, as C++ (none with TOOLCHAIN=ritteros), with the
# upstream projects' own compile options as their Linux builds use them;
# RitterOS's warnings are not theirs (no -Werror). Their headers go into
# $(UPSTREAM_INC) (the tree's builds) and /system/include.
ifneq ($(TOOLCHAIN),ritteros)
include user/upstream/versions
UPSTREAM_INC    := $(BUILD)/upstream/include
UPSTREAM_CFLAGS := -target $($(ARCH)_KERNEL_TARGET) -ffreestanding -nostdinc -isystem $(USER_SYSTEM_INC) \
                   -isystem user/include -fPIC -fno-stack-protector -fno-asynchronous-unwind-tables -O2
UPSTREAM_DEPS   := $(USER_HEADERS) user/upstream/versions $(TOOLCHAIN_STAMP) | $(ZIG_DEP)
UPSTREAM_HEADERS := $(UPSTREAM_INC)/zlib.h $(UPSTREAM_INC)/zconf.h
UPSTREAM_SYSTEM  := $(patsubst $(UPSTREAM_INC)/%,$(SYSTEM)/include/%,$(UPSTREAM_HEADERS)) \
                    $(SYSTEM)/doc/licences/zlib.txt $(SYSTEM)/doc/licences/SOURCE-zlib.txt \
                    $(BUILD)/upstream/openssl-headers \
                    $(SYSTEM)/doc/licences/openssl.txt $(SYSTEM)/doc/licences/SOURCE-openssl.txt \
                    $(BUILD)/upstream/netlibs-headers \
                    $(foreach l,nghttp2 brotli libpsl,$(SYSTEM)/doc/licences/$(l).txt $(SYSTEM)/doc/licences/SOURCE-$(l).txt) \
                    $(BUILD)/upstream/curl-headers $(SYSTEM)/doc/licences/curl.txt $(SYSTEM)/doc/licences/SOURCE-curl.txt
UPSTREAM_ZIG    := $(firstword $(wildcard $(abspath $(ZIG))) $(ZIG))
UPSTREAM_JOBS   := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

# zlib (46a): libz.so.1 from zlib's library files, as its configure builds
# them on Linux (-DHAVE_HIDDEN, unistd.h and stdarg.h there, its version
# script); the zconf.h installed says the same (configure's own edit).
ZLIB_SRC   := tools/src/zlib-$(ZLIB_VER)
ZLIB_OBJS  := $(patsubst %,$(OUT)/upstream/zlib/%.o,adler32 compress crc32 deflate gzclose gzlib gzread \
                                                   gzwrite infback inffast inflate inftrees trees uncompr zutil)
$(ZLIB_SRC)/zlib.h:
	@echo "$(ZLIB_SRC) is missing: run scripts/setup-toolchain.sh" >&2; exit 1
$(OUT)/upstream/zlib/%.o: $(ZLIB_SRC)/%.c $(ZLIB_SRC)/zlib.h $(UPSTREAM_DEPS)
	@mkdir -p $(dir $@)
	$(USER_CC) $(UPSTREAM_CFLAGS) -DHAVE_HIDDEN -DHAVE_UNISTD_H=1 -DHAVE_STDARG_H=1 -c $< -o $@
$(LIB_DIR_SYS)/libz.so.1: $(ZLIB_OBJS) $(LIB_DIR_SYS)/libritteros.so.1
	@mkdir -p $(dir $@)
	$(USER_LD) $(SO_LDFLAGS) -soname libz.so.1 --version-script $(ZLIB_SRC)/zlib.map -o $@ $(ZLIB_OBJS) \
	    $(LIB_DIR_SYS)/libritteros.so.1
$(UPSTREAM_INC)/zlib.h: $(ZLIB_SRC)/zlib.h
	@mkdir -p $(dir $@)
	cp $< $@
$(UPSTREAM_INC)/zconf.h: $(ZLIB_SRC)/zconf.h
	@mkdir -p $(dir $@)
	sed -e '/^#if HAVE_UNISTD_H-0.* may be/s/ HAVE_UNISTD_H-0\(.*\) may be/ 1\1 was/' \
	    -e '/^#if HAVE_STDARG_H-0.* may be/s/ HAVE_STDARG_H-0\(.*\) may be/ 1\1 was/' $< > $@
# (the licence kept in the tree must still be upstream's: an update that
# changes it stops the build until user/upstream's copy is looked at)
$(SYSTEM)/doc/licences/zlib.txt: user/upstream/LICENCE-zlib.txt $(ZLIB_SRC)/zlib.h
	@cmp -s $< $(ZLIB_SRC)/LICENSE || { echo "zlib's LICENSE differs from $<" >&2; exit 1; }
	@mkdir -p $(dir $@)
	cp $< $@

# OpenSSL (46b): libcrypto.so.3, libssl.so.3 and the openssl program, built
# by OpenSSL's own Configure and Makefile in $(OPENSSL_BUILD) for the target
# user/upstream/openssl.conf gives it (ritteros-x86_64, ritteros-aarch64):
# zig's clang with RitterOS's headers, linked by sdk/ritteros-ld as the
# SDK's builds are; the assembly on, no loadable modules (legacy provider,
# engines), OPENSSLDIR /system/config/openssl (no openssl.cnf: OpenSSL goes
# on without one; its cert.pem is mkdisk's link to /system/certs/cacert.pem).
# Configured again from nothing when its version, the target or RitterOS's
# headers change (its own dependencies leave -isystem headers out);
# otherwise its make runs again only when RitterOS's libraries changed.
# openssl-help.o is the program's PROGRAM_HELP line (bin_lflags).
OPENSSL_SRC   := tools/src/openssl-$(OPENSSL_VER)
OPENSSL_BUILD := $(OUT)/upstream/openssl
OPENSSL_STAMP := $(OPENSSL_BUILD)/ritteros-built
OPENSSL_HELP  := $(OUT)/upstream/openssl-help.o
$(OPENSSL_SRC)/Configure:
	@echo "$(OPENSSL_SRC) is missing: run scripts/setup-toolchain.sh" >&2; exit 1
$(OPENSSL_HELP): user/upstream/openssl-help.c $(USER_HEADERS) $(TOOLCHAIN_STAMP) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_PIC_CFLAGS) -c $< -o $@
$(OPENSSL_BUILD)/configdata.pm: $(OPENSSL_SRC)/Configure user/upstream/openssl.conf user/upstream/versions \
                                $(USER_HEADERS) $(OPENSSL_HELP)
	rm -rf $(OPENSSL_BUILD)
	@mkdir -p $(OPENSSL_BUILD)
	cd $(OPENSSL_BUILD) && RITTEROS_OPENSSL_HELP=$(abspath $(OPENSSL_HELP)) \
	    perl $(abspath $(OPENSSL_SRC))/Configure ritteros-$(ARCH) --config=$(abspath user/upstream/openssl.conf) \
	    --prefix=/system --openssldir=/system/config/openssl --libdir=$(ARCH)/lib \
	    enable-pie no-module no-legacy no-engine no-tests no-docs no-afalgeng \
	    CC="$(UPSTREAM_ZIG) clang" AR="$(UPSTREAM_ZIG) ar" RANLIB="$(UPSTREAM_ZIG) ranlib" \
	    CFLAGS="--target=$(ARCH)-unknown-none-elf -ffreestanding -nostdinc -isystem $(abspath $(USER_SYSTEM_INC)) \
	            -isystem $(abspath user/include) -fno-stack-protector -fno-asynchronous-unwind-tables -O2" \
	    LDFLAGS="--target=$(ARCH)-linux-gnu -nostdlib -fuse-ld=$(abspath sdk/ritteros-ld) \
	             -Wno-unused-command-line-argument" > configure.log 2>&1 || { cat configure.log; exit 1; }
$(OPENSSL_STAMP): $(OPENSSL_BUILD)/configdata.pm $(USER_START) $(USER_LIB_PIC) $(SHARED_LIBS) $(CXX_LIBS) \
                  $(LDSO) sdk/ritteros-ld user/user.ld
	$(MAKE) -C $(OPENSSL_BUILD) MAKEFLAGS= -j$(UPSTREAM_JOBS) build_sw > $(OPENSSL_BUILD)/build.log 2>&1 || \
	    { grep -E "error|Error" $(OPENSSL_BUILD)/build.log | head -30; exit 1; }
	@touch $@
$(LIB_DIR_SYS)/libcrypto.so.3 $(LIB_DIR_SYS)/libssl.so.3: $(OPENSSL_STAMP)
	@mkdir -p $(dir $@)
	cp $(OPENSSL_BUILD)/$(notdir $@) $@
$(SYSTEM)/$(ARCH)/bin/openssl: $(OPENSSL_STAMP)
	@mkdir -p $(dir $@)
	cp $(OPENSSL_BUILD)/apps/openssl $@
# The headers, one copy for both architectures: what each architecture's
# Configure generated must be the same (else the build stops).
$(BUILD)/upstream/openssl-headers: $(OPENSSL_STAMP)
	@for a in $(ARCHES); do d=$(BUILD)/$$a/upstream/openssl/include/openssl; \
	    [ ! -d $$d ] || diff -r $(OPENSSL_BUILD)/include/openssl $$d >/dev/null || \
	    { echo "OpenSSL's generated headers differ between $(ARCH) and $$a" >&2; exit 1; }; done
	rm -rf $(SYSTEM)/include/openssl $(UPSTREAM_INC)/openssl
	@mkdir -p $(SYSTEM)/include/openssl $(UPSTREAM_INC)/openssl $(dir $@)
	cp $(OPENSSL_SRC)/include/openssl/*.h $(OPENSSL_BUILD)/include/openssl/*.h $(SYSTEM)/include/openssl/
	cp $(SYSTEM)/include/openssl/*.h $(UPSTREAM_INC)/openssl/
	@touch $@
$(SYSTEM)/doc/licences/openssl.txt: user/upstream/LICENCE-openssl.txt $(OPENSSL_SRC)/Configure
	@cmp -s $< $(OPENSSL_SRC)/LICENSE.txt || { echo "OpenSSL's LICENSE.txt differs from $<" >&2; exit 1; }
	@mkdir -p $(dir $@)
	cp $< $@

# nghttp2, brotli and libpsl (46c): built by their own CMake (nghttp2,
# brotli) and Meson (libpsl) through the SDK's files (sdk/ritteros.cmake,
# sdk/ritteros.meson, a copy of it for the architecture): what a ported
# library's build does outside the tree. Built again from nothing when the
# version, RitterOS's headers or the SDK's files change (seconds); RitterOS's
# libraries only have to be there (a new libritteros.so.1 keeps their
# interface).
UPSTREAM_CMAKE := $(abspath tools/venv/bin/cmake) -G Ninja -DCMAKE_MAKE_PROGRAM=$(abspath tools/venv/bin/ninja) \
                  -DCMAKE_TOOLCHAIN_FILE=$(abspath sdk/ritteros.cmake) -DRITTEROS_ARCH=$(ARCH) -DCMAKE_BUILD_TYPE=Release
UPSTREAM_NINJA := $(abspath tools/venv/bin/ninja)
UPSTREAM_SDK   := sdk/ritteros.cmake sdk/cmake/Platform/RitterOS.cmake sdk/ritteros.meson sdk/ritteros-ld
UPSTREAM_LINKS := $(USER_START) $(USER_LIB_PIC) $(SHARED_LIBS) $(CXX_LIBS) $(LDSO) user/user.ld
NGHTTP2_SRC    := tools/src/nghttp2-$(NGHTTP2_VER)
NGHTTP2_BUILD  := $(OUT)/upstream/nghttp2
BROTLI_SRC     := tools/src/brotli-$(BROTLI_VER)
BROTLI_BUILD   := $(OUT)/upstream/brotli
PSL_SRC        := tools/src/libpsl-$(LIBPSL_VER)
PSL_BUILD      := $(OUT)/upstream/libpsl
$(NGHTTP2_SRC)/CMakeLists.txt $(BROTLI_SRC)/CMakeLists.txt $(PSL_SRC)/meson.build:
	@echo "$@ is missing: run scripts/setup-toolchain.sh" >&2; exit 1
$(NGHTTP2_BUILD)/ritteros-built: $(NGHTTP2_SRC)/CMakeLists.txt user/upstream/versions $(USER_HEADERS) $(UPSTREAM_SDK) \
                                 | $(UPSTREAM_LINKS)
	rm -rf $(dir $@) && mkdir -p $(dir $@)
	$(UPSTREAM_CMAKE) -S $(NGHTTP2_SRC) -B $(dir $@) -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=ON \
	    -DBUILD_STATIC_LIBS=OFF -DENABLE_DOC=OFF -DBUILD_TESTING=OFF > $(dir $@)configure.log 2>&1 || \
	    { cat $(dir $@)configure.log; exit 1; }
	$(UPSTREAM_NINJA) -C $(dir $@) > $(dir $@)build.log 2>&1 || { grep -E "error|FAILED" $(dir $@)build.log; exit 1; }
	@touch $@
$(BROTLI_BUILD)/ritteros-built: $(BROTLI_SRC)/CMakeLists.txt user/upstream/versions $(USER_HEADERS) $(UPSTREAM_SDK) \
                                | $(UPSTREAM_LINKS)
	rm -rf $(dir $@) && mkdir -p $(dir $@)
	$(UPSTREAM_CMAKE) -S $(BROTLI_SRC) -B $(dir $@) -DBUILD_SHARED_LIBS=ON -DBROTLI_DISABLE_TESTS=ON \
	    -DBROTLI_BUILD_TOOLS=OFF > $(dir $@)configure.log 2>&1 || \
	    { cat $(dir $@)configure.log; exit 1; }
	$(UPSTREAM_NINJA) -C $(dir $@) > $(dir $@)build.log 2>&1 || { grep -E "error|FAILED" $(dir $@)build.log; exit 1; }
	@touch $@
$(PSL_BUILD)/ritteros-built: $(PSL_SRC)/meson.build user/upstream/versions $(USER_HEADERS) $(UPSTREAM_SDK) \
                             | $(UPSTREAM_LINKS)
	rm -rf $(dir $@) && mkdir -p $(dir $@)
	sed -e "s/^arch = .*/arch = '$(ARCH)'/" -e "s|^ritteros = .*|ritteros = '$(CURDIR)'|" sdk/ritteros.meson \
	    > $(OUT)/upstream/ritteros.meson
	PATH=$(abspath tools/venv/bin):$$PATH meson setup $(dir $@) $(PSL_SRC) --cross-file $(OUT)/upstream/ritteros.meson \
	    --buildtype=release -Ddefault_library=shared -Druntime=no -Dbuiltin=true -Dtests=false -Ddocs=false \
	    > $(OUT)/upstream/libpsl-configure.log 2>&1 || { cat $(OUT)/upstream/libpsl-configure.log; exit 1; }
	$(UPSTREAM_NINJA) -C $(dir $@) > $(dir $@)build.log 2>&1 || { grep -E "error|FAILED" $(dir $@)build.log; exit 1; }
	@touch $@
$(LIB_DIR_SYS)/libnghttp2.so.14: $(NGHTTP2_BUILD)/ritteros-built
	@mkdir -p $(dir $@)
	cp $(NGHTTP2_BUILD)/lib/$(notdir $@) $@
$(addprefix $(LIB_DIR_SYS)/,libbrotlicommon.so.1 libbrotlidec.so.1 libbrotlienc.so.1): $(BROTLI_BUILD)/ritteros-built
	@mkdir -p $(dir $@)
	cp $(BROTLI_BUILD)/$(notdir $@) $@
$(LIB_DIR_SYS)/libpsl.so.5: $(PSL_BUILD)/ritteros-built
	@mkdir -p $(dir $@)
	cp $(PSL_BUILD)/src/$(notdir $@) $@
# Their headers, one copy for both architectures (what each build generated
# must be the same): nghttp2/, brotli/, libpsl.h.
NETLIBS_GENERATED := nghttp2/lib/includes/nghttp2/nghttp2ver.h libpsl/include/libpsl.h
$(BUILD)/upstream/netlibs-headers: $(NGHTTP2_BUILD)/ritteros-built $(BROTLI_BUILD)/ritteros-built $(PSL_BUILD)/ritteros-built
	@for a in $(ARCHES); do for f in $(NETLIBS_GENERATED); do [ ! -f $(BUILD)/$$a/upstream/$$f ] || \
	    cmp -s $(OUT)/upstream/$$f $(BUILD)/$$a/upstream/$$f || \
	    { echo "$$f differs between $(ARCH) and $$a" >&2; exit 1; }; done; done
	rm -rf $(SYSTEM)/include/nghttp2 $(SYSTEM)/include/brotli $(UPSTREAM_INC)/nghttp2 $(UPSTREAM_INC)/brotli
	@mkdir -p $(SYSTEM)/include/nghttp2 $(SYSTEM)/include/brotli $(UPSTREAM_INC)/nghttp2 $(UPSTREAM_INC)/brotli \
	    $(dir $@)
	cp $(NGHTTP2_SRC)/lib/includes/nghttp2/nghttp2.h $(NGHTTP2_BUILD)/lib/includes/nghttp2/nghttp2ver.h \
	    $(SYSTEM)/include/nghttp2/
	cp $(BROTLI_SRC)/c/include/brotli/*.h $(SYSTEM)/include/brotli/
	cp $(PSL_BUILD)/include/libpsl.h $(SYSTEM)/include/libpsl.h
	cp $(SYSTEM)/include/nghttp2/*.h $(UPSTREAM_INC)/nghttp2/
	cp $(SYSTEM)/include/brotli/*.h $(UPSTREAM_INC)/brotli/
	cp $(SYSTEM)/include/libpsl.h $(UPSTREAM_INC)/libpsl.h
	@touch $@
# (the licences kept in the tree must still be upstream's; libpsl's is its
# own and Chromium's, whose code it has)
$(SYSTEM)/doc/licences/nghttp2.txt: user/upstream/LICENCE-nghttp2.txt $(NGHTTP2_SRC)/CMakeLists.txt
	@cmp -s $< $(NGHTTP2_SRC)/COPYING || { echo "nghttp2's COPYING differs from $<" >&2; exit 1; }
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/doc/licences/brotli.txt: user/upstream/LICENCE-brotli.txt $(BROTLI_SRC)/CMakeLists.txt
	@cmp -s $< $(BROTLI_SRC)/LICENSE || { echo "brotli's LICENSE differs from $<" >&2; exit 1; }
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/doc/licences/libpsl.txt: user/upstream/LICENCE-libpsl.txt $(PSL_SRC)/meson.build
	@cat $(PSL_SRC)/LICENSE $(PSL_SRC)/src/LICENSE.chromium | cmp -s $< - || \
	    { echo "libpsl's LICENSE and LICENSE.chromium differ from $<" >&2; exit 1; }
	@mkdir -p $(dir $@)
	cp $< $@

# curl (46d-2): libcurl.so.4 and the curl program, by curl's own CMake
# through the SDK's file, against the system libraries above (each named by
# its path: their pkg-config files and link names are 46e's), their headers
# from $(UPSTREAM_STAGE) (only theirs: /system/include also has our cc's
# own headers). OpenSSL, nghttp2 (HTTP/2), zlib, brotli, libpsl; the
# threaded resolver, IPv6, cookies, proxies, WebSockets; the CA bundle
# /system/certs/cacert.pem; libcurl's soname curl's Linux one (its CMake
# gives one only to the systems it knows). curl-help.o is the program's
# PROGRAM_HELP line.
UPSTREAM_STAGE := $(OUT)/upstream/include
CURL_SRC   := tools/src/curl-$(CURL_VER)
CURL_BUILD := $(OUT)/upstream/curl
CURL_HELP  := $(OUT)/upstream/curl-help.o
CURL_DEPS  := $(addprefix $(LIB_DIR_SYS)/,libssl.so.3 libcrypto.so.3 libz.so.1 libbrotlicommon.so.1 libbrotlidec.so.1 \
                                          libnghttp2.so.14 libpsl.so.5)
$(CURL_SRC)/CMakeLists.txt:
	@echo "$@ is missing: run scripts/setup-toolchain.sh" >&2; exit 1
$(OUT)/upstream/include-staged: $(UPSTREAM_HEADERS) $(OPENSSL_STAMP) $(NGHTTP2_BUILD)/ritteros-built \
                                $(BROTLI_BUILD)/ritteros-built $(PSL_BUILD)/ritteros-built
	rm -rf $(UPSTREAM_STAGE)
	@mkdir -p $(UPSTREAM_STAGE)/openssl $(UPSTREAM_STAGE)/nghttp2 $(UPSTREAM_STAGE)/brotli
	cp $(UPSTREAM_HEADERS) $(PSL_BUILD)/include/libpsl.h $(UPSTREAM_STAGE)/
	cp $(OPENSSL_SRC)/include/openssl/*.h $(OPENSSL_BUILD)/include/openssl/*.h $(UPSTREAM_STAGE)/openssl/
	cp $(NGHTTP2_SRC)/lib/includes/nghttp2/nghttp2.h $(NGHTTP2_BUILD)/lib/includes/nghttp2/nghttp2ver.h \
	    $(UPSTREAM_STAGE)/nghttp2/
	cp $(BROTLI_SRC)/c/include/brotli/*.h $(UPSTREAM_STAGE)/brotli/
	@touch $@
$(CURL_HELP): user/upstream/curl-help.c $(USER_HEADERS) $(TOOLCHAIN_STAMP) | $(ZIG_DEP)
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_PIC_CFLAGS) -c $< -o $@
$(CURL_BUILD)/ritteros-built: $(CURL_SRC)/CMakeLists.txt user/upstream/versions $(USER_HEADERS) $(UPSTREAM_SDK) \
                              $(OUT)/upstream/include-staged $(CURL_DEPS) $(CURL_HELP) | $(UPSTREAM_LINKS)
	rm -rf $(dir $@) && mkdir -p $(dir $@)
	$(UPSTREAM_CMAKE) -S $(CURL_SRC) -B $(dir $@) -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
	    -DBUILD_CURL_EXE=ON -DCURL_LIBCURL_SOVERSION=ON -DCURL_USE_PKGCONFIG=OFF \
	    -DCMAKE_EXE_LINKER_FLAGS=$(abspath $(CURL_HELP)) \
	    -DCURL_USE_OPENSSL=ON -DOPENSSL_INCLUDE_DIR=$(abspath $(UPSTREAM_STAGE)) \
	    -DOPENSSL_SSL_LIBRARY=$(abspath $(LIB_DIR_SYS))/libssl.so.3 \
	    -DOPENSSL_CRYPTO_LIBRARY=$(abspath $(LIB_DIR_SYS))/libcrypto.so.3 \
	    -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR=$(abspath $(UPSTREAM_STAGE)) -DZLIB_LIBRARY=$(abspath $(LIB_DIR_SYS))/libz.so.1 \
	    -DCURL_BROTLI=ON -DBROTLI_INCLUDE_DIR=$(abspath $(UPSTREAM_STAGE)) \
	    -DBROTLICOMMON_LIBRARY=$(abspath $(LIB_DIR_SYS))/libbrotlicommon.so.1 \
	    -DBROTLIDEC_LIBRARY=$(abspath $(LIB_DIR_SYS))/libbrotlidec.so.1 \
	    -DUSE_NGHTTP2=ON -DNGHTTP2_INCLUDE_DIR=$(abspath $(UPSTREAM_STAGE)) \
	    -DNGHTTP2_LIBRARY=$(abspath $(LIB_DIR_SYS))/libnghttp2.so.14 \
	    -DCURL_USE_LIBPSL=ON -DLIBPSL_INCLUDE_DIR=$(abspath $(UPSTREAM_STAGE)) \
	    -DLIBPSL_LIBRARY=$(abspath $(LIB_DIR_SYS))/libpsl.so.5 \
	    -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF \
	    -DUSE_NGTCP2=OFF -DCURL_DISABLE_LDAP=ON -DENABLE_THREADED_RESOLVER=ON -DENABLE_IPV6=ON \
	    -DCURL_CA_BUNDLE=/system/certs/cacert.pem -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF \
	    -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
	    -DBUILD_EXAMPLES=OFF > $(dir $@)configure.log 2>&1 || { cat $(dir $@)configure.log; exit 1; }
	$(UPSTREAM_NINJA) -C $(dir $@) > $(dir $@)build.log 2>&1 || { grep -E "error|FAILED" $(dir $@)build.log; exit 1; }
	@touch $@
$(LIB_DIR_SYS)/libcurl.so.4: $(CURL_BUILD)/ritteros-built
	@mkdir -p $(dir $@)
	cp $(CURL_BUILD)/lib/libcurl.so.4 $@
$(SYSTEM)/$(ARCH)/bin/curl: $(CURL_BUILD)/ritteros-built
	@mkdir -p $(dir $@)
	cp $(CURL_BUILD)/src/curl $@
$(BUILD)/upstream/curl-headers: $(CURL_BUILD)/ritteros-built
	rm -rf $(SYSTEM)/include/curl $(UPSTREAM_INC)/curl
	@mkdir -p $(SYSTEM)/include/curl $(UPSTREAM_INC)/curl $(dir $@)
	cp $(CURL_SRC)/include/curl/*.h $(SYSTEM)/include/curl/
	cp $(CURL_SRC)/include/curl/*.h $(UPSTREAM_INC)/curl/
	@touch $@
# (46e) For builds outside the tree (the SDK): every upstream library's
# headers in $(UPSTREAM_INC), one folder for both architectures (only
# theirs, without our cc's own headers /system/include also has), and link
# names in $(BUILD)/sdk/ARCH/lib: libssl.so for libssl.so.3 ... (links to
# the system's files), which -lssl and CMake's find_package look for. The
# disk has neither: on RitterOS /system/include holds the headers.
SDK_LINK_DIR := $(BUILD)/sdk/$(ARCH)/lib
SDK_LINKS    := $(foreach l,$(UPSTREAM_LIBS),$(SDK_LINK_DIR)/$(basename $(notdir $(l))))
define sdk_link
$(SDK_LINK_DIR)/$(basename $(notdir $(1))): $(1)
	@mkdir -p $$(dir $$@)
	ln -sf ../../../system/$(ARCH)/lib/$(notdir $(1)) $$@
endef
$(foreach l,$(UPSTREAM_LIBS),$(eval $(call sdk_link,$(l))))
# (71b, the browser agent's request 6) -lm, -lpthread, -ldl, -lrt: empty
# archives beside the link names (as Linux and the BSDs ship them): what
# those libraries hold elsewhere is libritteros's.
SDK_STUBS    := $(addprefix $(SDK_LINK_DIR)/,libm.a libpthread.a libdl.a librt.a)
$(SDK_STUBS):
	@mkdir -p $(dir $@)
	rm -f $@ && $(ZIG) ar rcs $@
UPSTREAM_SDK_FILES := $(UPSTREAM_HEADERS) $(BUILD)/upstream/openssl-headers $(BUILD)/upstream/netlibs-headers \
                      $(BUILD)/upstream/curl-headers $(SDK_LINKS) $(SDK_STUBS)
sdk-arch: $(UPSTREAM_SDK_FILES)
$(SYSTEM)/doc/licences/curl.txt: user/upstream/LICENCE-curl.txt $(CURL_SRC)/CMakeLists.txt
	@cmp -s $< $(CURL_SRC)/COPYING || { echo "curl's COPYING differs from $<" >&2; exit 1; }
	@mkdir -p $(dir $@)
	cp $< $@

$(SYSTEM)/include/%.h: $(UPSTREAM_INC)/%.h
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/doc/licences/SOURCE-%.txt: user/upstream/SOURCE-%.txt
	@mkdir -p $(dir $@)
	cp $< $@

# Their test programs (user/test, UPSTREAM_TESTS above): with the headers
# and the libraries they test.
$(OUT)/pic/user/test/zlibtest.c.o: $(UPSTREAM_HEADERS)
$(OUT)/pic/user/test/zlibtest.c.o: USER_PIC_CFLAGS += -isystem $(UPSTREAM_INC)
$(SYSTEM)/$(ARCH)/test/zlibtest: $(LIB_DIR_SYS)/libz.so.1
$(SYSTEM)/$(ARCH)/test/zlibtest: PARTS = $(LIB_DIR_SYS)/libz.so.1
$(OUT)/pic/user/test/openssltest.c.o: $(OPENSSL_STAMP)
$(OUT)/pic/user/test/openssltest.c.o: USER_PIC_CFLAGS += -isystem $(OPENSSL_BUILD)/include -isystem $(OPENSSL_SRC)/include
$(SYSTEM)/$(ARCH)/test/openssltest: $(LIB_DIR_SYS)/libssl.so.3 $(LIB_DIR_SYS)/libcrypto.so.3
$(SYSTEM)/$(ARCH)/test/openssltest: PARTS = $(LIB_DIR_SYS)/libssl.so.3 $(LIB_DIR_SYS)/libcrypto.so.3
NETLIBS_TEST_LIBS := $(addprefix $(LIB_DIR_SYS)/,libnghttp2.so.14 libbrotlidec.so.1 libbrotlienc.so.1 \
                                                 libbrotlicommon.so.1 libpsl.so.5)
$(OUT)/pic/user/test/netlibstest.c.o: $(NGHTTP2_BUILD)/ritteros-built $(BROTLI_BUILD)/ritteros-built $(PSL_BUILD)/ritteros-built
$(OUT)/pic/user/test/netlibstest.c.o: USER_PIC_CFLAGS += -isystem $(NGHTTP2_SRC)/lib/includes \
    -isystem $(NGHTTP2_BUILD)/lib/includes -isystem $(BROTLI_SRC)/c/include -isystem $(PSL_BUILD)/include
$(SYSTEM)/$(ARCH)/test/netlibstest: $(NETLIBS_TEST_LIBS)
$(SYSTEM)/$(ARCH)/test/netlibstest: PARTS = $(NETLIBS_TEST_LIBS)
$(OUT)/pic/user/test/curltest.c.o: $(CURL_BUILD)/ritteros-built
$(OUT)/pic/user/test/curltest.c.o: USER_PIC_CFLAGS += -isystem $(CURL_SRC)/include
$(SYSTEM)/$(ARCH)/test/curltest: $(LIB_DIR_SYS)/libcurl.so.4
$(SYSTEM)/$(ARCH)/test/curltest: PARTS = $(LIB_DIR_SYS)/libcurl.so.4
endif

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

# (43c) The C++ libraries' licences (Apache-2.0 with the LLVM exception; the
# sources are the pinned zig's: user/cxx/SOURCE.txt), with the documentation.
SYSTEM_LICENCES := $(SYSTEM)/doc/licences/libc++.txt $(SYSTEM)/doc/licences/libc++abi.txt \
                   $(SYSTEM)/doc/licences/libunwind.txt $(SYSTEM)/doc/licences/SOURCE-c++.txt
$(SYSTEM)/doc/licences/libc++.txt: $(ZIG_LIB)/libcxx/LICENSE.TXT
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/doc/licences/libc++abi.txt: $(ZIG_LIB)/libcxxabi/LICENSE.TXT
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/doc/licences/libunwind.txt: $(ZIG_LIB)/libunwind/LICENSE.TXT
	@mkdir -p $(dir $@)
	cp $< $@
$(SYSTEM)/doc/licences/SOURCE-c++.txt: user/cxx/SOURCE.txt
	@mkdir -p $(dir $@)
	cp $< $@

disk: $(SYSTEM_FONTS) $(SYSTEM_CERTS) $(SYSTEM_FIRMWARE) $(SYSTEM_INCLUDE) $(SYSTEM_SDK) $(SYSTEM_DOC) $(SYSTEM_LICENCES) \
      $(UPSTREAM_SYSTEM) $(DISKS)

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
