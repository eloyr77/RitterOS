# A RitterOS program, built with RitterOS's SDK (RITTEROS/sdk/CLAUDE.md).
#
#   make                                 build build/x86_64/hello
#   make deploy run HOST=root@ADDRESS    copy it to the RitterOS machine and run it
#   make stop HOST=root@ADDRESS          end it there
#   make ARCH=aarch64 ...                the same for 64-bit ARM (QEMU)
#
# HOST (and SSH_OPTS, e.g. SSH_OPTS="-i ~/.ssh/ritteros") can also be set in
# the environment instead of on every command line.
#
# On RitterOS itself the same Makefile builds with the machine's own cc (no
# checkout needed: it takes /system/sdk/ritteros.mk); there `make deploy run`
# installs and runs it on that machine, without HOST.

RITTEROS ?= ../RitterOS
APP      := hello
KIND     := bin
SRCS     := hello.c
CFLAGS   += -Werror

RITTEROS_MK := $(firstword $(wildcard $(RITTEROS)/sdk/ritteros.mk /system/sdk/ritteros.mk))
ifeq ($(RITTEROS_MK),)
$(error no RitterOS SDK: neither a checkout at RITTEROS=$(RITTEROS) (git clone https://github.com/eloyr77/RitterOS.git next to this project, or make RITTEROS=/path/to/RitterOS) nor /system/sdk/ritteros.mk (RitterOS itself, since 63f))
endif
include $(RITTEROS_MK)
