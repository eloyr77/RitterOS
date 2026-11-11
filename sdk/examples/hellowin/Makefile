# hellowin: a windowed RitterOS app (KIND := apps), built with the SDK. Inside
# the RitterOS tree, RITTEROS is three levels up; copied out, set it. On
# RitterOS itself it takes the machine's /system/sdk/ritteros.mk.
#
#   make deploy run HOST=root@ADDRESS    (windowd must be running on that machine)

RITTEROS ?= ../../..
APP      := hellowin
KIND     := apps
SRCS     := hellowin.c
CFLAGS   += -Werror

include $(firstword $(wildcard $(RITTEROS)/sdk/ritteros.mk /system/sdk/ritteros.mk) $(RITTEROS)/sdk/ritteros.mk)
