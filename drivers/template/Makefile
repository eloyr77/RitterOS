# The template driver's Makefile: a driver outside RitterOS's tree
# (drivers/driver.mk). `make` builds build/x86_64/example.mod, `make
# ARCH=aarch64` the ARM one. Point RITTEROS at a RitterOS checkout.
DRIVER   := example
SRCS     := example.c
RITTEROS ?= ../..
include $(RITTEROS)/drivers/driver.mk
