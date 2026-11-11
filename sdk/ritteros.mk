# ritteros.mk: build a RitterOS program outside the RitterOS tree, copy it to
# a running RitterOS machine over ssh, and run or stop it there.
#
# A project's Makefile sets a few variables and includes this file:
#
#   RITTEROS ?= ../RitterOS
#   APP      := hello
#   KIND     := bin
#   SRCS     := hello.c
#   include $(RITTEROS)/sdk/ritteros.mk
#
# RITTEROS is a RitterOS checkout (the source tree), APP the program's name
# on RitterOS, KIND bin (a command-line program) or apps (a windowed one),
# SRCS its C sources (default: APP.c). No comments after a value on the same
# line: make keeps the spaces before the # in the value.
#
# Then:
#   make                     build build/ARCH/APP (ARCH: x86_64, the default, or aarch64;
#                            LINK: shared, the default, or static)
#   make deploy HOST=...     copy it to /system/ARCH/KIND/APP there and make it executable
#                            (apps, 42b: into its bundle /system/ARCH/apps/APP.program/,
#                            with APP.info and APP.png, if the project has them, as the
#                            bundle's info and icon.png); (41h-7) one ssh call through
#                            sudo: root or a user in sudo's `nopassword` list is not
#                            asked, anyone else types the password once
#   make run HOST=...        run it there (ARGS="..." for its arguments); its output
#                            and exit code come back
#   make stop HOST=...       end it there (kill, the process numbers from ps)
#   make clean
#
# HOST is ssh's user@address (root@192.168.1.50); SSH_OPTS adds ssh options
# (-p 2222, -i ~/.ssh/ritteros). The C library and the start code are
# RitterOS's own, from its source: `make sdk` in RITTEROS, which this runs
# (it does nothing when they are up to date). See sdk/CLAUDE.md.
#
# On RitterOS itself (63f) this file is /system/sdk/ritteros.mk, and the same
# project builds there with the machine's own cc and ld, its headers
# (/system/include) and libraries (/system/ARCH/lib): no checkout, no Zig, no
# HOST. deploy copies the program to /system/ARCH/KIND/APP on this machine,
# run runs it here, stop ends it here; ARCH is this machine's.

ON_RITTEROS := $(if $(wildcard /system/include/ritteros.h),$(if $(wildcard /system/sdk/ritteros.mk),yes))
ifeq ($(ON_RITTEROS),yes)
MACHINE_ARCH := $(patsubst /system/%/lib,%,$(firstword $(wildcard /system/x86_64/lib /system/aarch64/lib)))
ARCH ?= $(MACHINE_ARCH)
else
ifndef RITTEROS
$(error set RITTEROS to a RitterOS checkout before including ritteros.mk)
endif
endif
ifndef APP
$(error set APP to the program's name before including ritteros.mk)
endif
KIND ?= bin
ARCH ?= x86_64
SRCS ?= $(APP).c
LINK ?= shared
# (stray spaces, e.g. before a comment on the same line, never reach a path)
override APP  := $(strip $(APP))
override KIND := $(strip $(KIND))
override ARCH := $(strip $(ARCH))
override LINK := $(strip $(LINK))
ifeq ($(filter $(LINK),shared static),)
$(error LINK must be shared (RitterOS's shared libraries, the default) or static, not '$(LINK)')
endif
ifeq ($(filter $(KIND),bin apps),)
$(error KIND must be bin (command line) or apps (windowed), not '$(KIND)')
endif
ifeq ($(filter $(ARCH),x86_64 aarch64),)
$(error ARCH must be x86_64 or aarch64, not '$(ARCH)')
endif

ifneq ($(ON_RITTEROS),yes)
RITTEROS_DIR := $(abspath $(RITTEROS))
ifeq ($(wildcard $(RITTEROS_DIR)/user/include/ritteros.h),)
$(error RITTEROS=$(RITTEROS) is not a RitterOS checkout (no user/include/ritteros.h))
endif

# Zig (its clang and lld are the cross compiler): RitterOS's own copy if
# scripts/setup-toolchain.sh made one, else zig from PATH. RitterOS pins 0.17.0.
ZIG ?= $(firstword $(wildcard $(RITTEROS_DIR)/tools/zig/zig) zig)
ZIG_EXE := $(abspath $(shell command -v $(ZIG) 2>/dev/null))
ifeq ($(ZIG_EXE),)
$(error no zig: install Zig 0.17.0 (https://ziglang.org/download/) or set ZIG=/path/to/zig)
endif
# Zig's own headers (stdint.h, stddef.h, stdarg.h, ...), wherever zig lives.
ZIG_LIB := $(abspath $(shell $(ZIG_EXE) env | sed -n 's/^ *\.lib_dir = "\(.*\)",$$/\1/p'))

# The same flags RitterOS builds its own programs with: a freestanding ELF,
# RitterOS's headers instead of a host's C library, position-independent
# (22l: the system puts a program at a random place; a static link takes the
# same objects). Add your own with CFLAGS (the template adds -Werror).
TARGET_x86_64  := x86_64-unknown-none-elf
TARGET_aarch64 := aarch64-unknown-none-elf
RITTEROS_CFLAGS := -target $(TARGET_$(ARCH)) -ffreestanding -nostdinc \
                   -isystem $(ZIG_LIB)/include \
                   -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables \
                   -O2 -std=c11 -Wall -Wextra
# RitterOS's headers come after your CFLAGS (63e): your own -I folders are
# searched first, so a header of yours named like one of RitterOS's wins.
RITTEROS_INCLUDE := -I$(RITTEROS_DIR)/user/include
RITTEROS_LIB   := $(RITTEROS_DIR)/build/$(ARCH)/user/libritteros.a
RITTEROS_LIB_PIC := $(RITTEROS_DIR)/build/$(ARCH)/user/libritteros-pic.a
RITTEROS_START := $(RITTEROS_DIR)/build/$(ARCH)/user/user/arch/$(ARCH)/start.S.o
# LINK = shared (22d): like RitterOS's own programs, against its shared
# libraries (/system/ARCH/lib on the machine; ld.so maps them at the start,
# --as-needed records only the ones the program uses) and libritteros-pic.a
# for the static-only parts, position-independent (-pie, 22l). LINK = static:
# everything from libritteros.a, so the program carries the code it uses (at
# a fixed place).
RITTEROS_SHARED := $(addprefix $(RITTEROS_DIR)/build/system/$(ARCH)/lib/,libritteros.so.1 \
                     libwindow.so.1 libimage.so.1 libaudio.so.1 libvideo.so.1 libdraw.so.1 libwidget.so.1 \
                     libtls.so.1)
ifeq ($(LINK),shared)
RITTEROS_LDFLAGS := -T $(RITTEROS_DIR)/user/user.ld -nostdlib -z max-page-size=4096 -pie \
                    -dynamic-linker /system/$(ARCH)/lib/ld.so -z now -z relro -z nocopyreloc \
                    --hash-style=sysv
RITTEROS_LIBS    := --as-needed $(RITTEROS_SHARED) --no-as-needed $(RITTEROS_LIB_PIC)
else
RITTEROS_LDFLAGS := -T $(RITTEROS_DIR)/user/user.ld -nostdlib -static -z max-page-size=4096 -z norelro
RITTEROS_LIBS    := $(RITTEROS_LIB)
endif
else
# On RitterOS itself (63f): the machine's own compiler and linker, linked as
# cc links (the start code, the shared libraries the program uses, then the
# static-only parts), or statically. The headers are /system/include.
ifneq ($(ARCH),$(MACHINE_ARCH))
$(error this machine builds for $(MACHINE_ARCH) only, not ARCH=$(ARCH))
endif
MACHINE_LIB := /system/$(ARCH)/lib
ifeq ($(LINK),shared)
MACHINE_LIBS := --as-needed $(addprefix $(MACHINE_LIB)/,libritteros.so.1 libwindow.so.1 \
                  libimage.so.1 libaudio.so.1 libvideo.so.1 libdraw.so.1 libwidget.so.1 libtls.so.1) \
                --no-as-needed $(MACHINE_LIB)/libritteros.a
MACHINE_LDFLAGS :=
else
MACHINE_LIBS := $(MACHINE_LIB)/libritteros.a
MACHINE_LDFLAGS := -static
endif
endif

OUT  := build/$(ARCH)
OBJS := $(patsubst %.c,$(OUT)/obj/%.o,$(SRCS))
PROGRAM := $(OUT)/$(APP)
ifeq ($(KIND),apps)
BUNDLE := /system/$(ARCH)/apps/$(APP).program
DEST := $(BUNDLE)/$(APP)
else
DEST := /system/$(ARCH)/$(KIND)/$(APP)
endif
APP_FILES := $(if $(BUNDLE),$(wildcard $(APP).info $(APP).png))
# (41h-7) What deploy installs, laid out as under /system/ARCH/KIND: the
# program, or the app's bundle (the program, info, icon.png).
STAGE := $(OUT)/stage
STAGED := $(STAGE)/$(if $(BUNDLE),$(APP).program/)$(APP)
STAGE_TOP := $(if $(BUNDLE),$(APP).program,$(APP))
UNPACK := $(if $(BUNDLE),rm -f /system/$(ARCH)/apps/$(APP); )tar xf - -C /system/$(ARCH)/$(KIND)

# ssh never waits for a typed password (an agent cannot type one; a missing
# key fails at once) and trusts a machine's host key the first time.
SSH := ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new $(SSH_OPTS)
need_host = $(if $(HOST),,$(error set HOST=user@address, e.g. make $@ HOST=root@192.168.1.50))

.PHONY: all deploy run stop clean ritteros-sdk stage
all: $(PROGRAM)

ifneq ($(ON_RITTEROS),yes)
# RitterOS's libraries (shared and static) and start code, built in its own
# tree (both architectures).
ritteros-sdk:
	@$(MAKE) --no-print-directory -C $(RITTEROS_DIR) sdk ZIG=$(ZIG_EXE)

$(OUT)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(ZIG_EXE) clang $(RITTEROS_CFLAGS) $(CFLAGS) $(RITTEROS_INCLUDE) -MMD -MP -c $< -o $@

$(PROGRAM): $(OBJS) ritteros-sdk
	$(ZIG_EXE) ld.lld $(RITTEROS_LDFLAGS) -o $@ $(RITTEROS_START) $(OBJS) $(RITTEROS_LIBS)
else
ritteros-sdk:

$(OUT)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	cc $(CFLAGS) -MMD -MP -c $< -o $@

$(PROGRAM): $(OBJS)
	ld $(MACHINE_LDFLAGS) -o $@ $(MACHINE_LIB)/start.o $(OBJS) $(MACHINE_LIBS)
endif

# (41h-7) The files deploy installs, staged as they go.
stage: $(PROGRAM)
	rm -rf $(STAGE)
	mkdir -p $(dir $(STAGED))
	cp $(PROGRAM) $(STAGED)
	chmod 755 $(STAGED)
	$(if $(filter $(APP).info,$(APP_FILES)),cp $(APP).info $(STAGE)/$(APP).program/info)
	$(if $(filter $(APP).png,$(APP_FILES)),cp $(APP).png $(STAGE)/$(APP).program/icon.png)

ifeq ($(ON_RITTEROS),yes)
# Here on the machine: unpacked by root, or (41h-7) through sudo (it asks
# at the terminal, its standard input: the archive is read by its path).
# (41h-9) Root is told by the shell's case (RitterOS's grep had no -q
# before 68f), its pattern unquoted (a partly quoted one was literal there).
deploy: stage
	tar cf $(OUT)/deploy.tar -C $(STAGE) $(STAGE_TOP)
	case "$$(id)" in uid=0[!0-9]*) s=;; *) s=sudo;; esac; \
	$$s sh -c '$(subst tar xf -,tar xf $(OUT)/deploy.tar,$(UNPACK))'
	@echo "deployed $(PROGRAM) to $(DEST)"

run:
	$(APP) $(ARGS)

stop:
	@pids=$$(ps | awk '$$NF == "$(APP)" && $$4 == "running" { print $$1 }'); \
	if [ -z "$$pids" ]; then echo "$(APP) is not running"; \
	else echo "kill $$pids"; kill $$pids; fi
else
# (41h-7) One ssh call, through sudo, the files as a tar stream: root and
# the users in sudo's `nopassword` list are not asked; anyone else types
# the password here once (hidden), and it goes as sudo's first input line
# (the rest is tar's). A user sudo refuses is told so at once; nothing
# typed (no terminal: an agent) stops here, never handing sudo the archive.
deploy: stage
	$(need_host)
	@pw=; if ! said=$$($(SSH) $(HOST) 'sudo true' </dev/null 2>&1); then \
	    case "$$said" in *"may not run commands as root"*) \
	        echo "$$said" | grep 'may not run'; exit 1;; esac; \
	    printf 'sudo password on $(HOST): '; stty -echo 2>/dev/null; read -r pw; \
	    stty echo 2>/dev/null; echo; \
	    if [ -z "$$pw" ]; then \
	        echo "make deploy: $(HOST) needs a password for sudo: type it here, or root lists" \
	             "the user in /system/config/sudo's nopassword"; exit 1; fi; fi; \
	{ if [ -n "$$pw" ]; then printf '%s\n' "$$pw"; fi; \
	  COPYFILE_DISABLE=1 tar cf - -C $(STAGE) $(STAGE_TOP); } | $(SSH) $(HOST) 'sudo sh -c "$(UNPACK)"'
	@echo "deployed $(PROGRAM) to $(HOST):$(DEST)"

# A windowed app keeps the session open (and its output coming) until its
# window closes; run it in the background, `make stop` ends it.
run:
	$(need_host)
	$(SSH) $(HOST) '$(APP) $(ARGS)'

# ps lists "pid parent group state name" (state new, running or ended; the
# name is the program's file name).
stop:
	$(need_host)
	@pids=$$($(SSH) $(HOST) ps | awk '$$NF == "$(APP)" && $$4 == "running" { print $$1 }'); \
	if [ -z "$$pids" ]; then echo "$(APP) is not running on $(HOST)"; \
	else echo "kill $$pids"; $(SSH) $(HOST) "kill $$pids"; fi
endif

clean:
	rm -rf build

-include $(OBJS:.o=.d)
