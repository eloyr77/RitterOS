# widgets: a windowed RitterOS app built from libwidget's controls (41e),
# with the SDK. Inside the RitterOS tree, RITTEROS is three levels up;
# copied out, set it. On RitterOS itself it takes the machine's
# /system/sdk/ritteros.mk.
#
#   make deploy run HOST=root@ADDRESS    (windowd must be running on that machine)

RITTEROS ?= ../../..
APP      := widgets
KIND     := apps
SRCS     := widgets.c
CFLAGS   += -Werror

include $(firstword $(wildcard $(RITTEROS)/sdk/ritteros.mk /system/sdk/ritteros.mk) $(RITTEROS)/sdk/ritteros.mk)
