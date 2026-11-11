# notes: a note kept in ~/notes.txt, a windowed RitterOS app with its app
# file and a test without a screen (41h-4), built with the SDK. Inside the
# RitterOS tree, RITTEROS is three levels up; copied out, set it. On
# RitterOS itself it takes the machine's /system/sdk/ritteros.mk.
#
#   make deploy HOST=root@ADDRESS        its bundle /system/ARCH/apps/notes.program/
#                                        (the program, notes.info as its info)
#   then its test, with no screen needed (see test.sh):
#     cat test.sh | ssh HOST 'cat > /tmp/notes-test.sh'; ssh HOST sh /tmp/notes-test.sh

RITTEROS ?= ../../..
APP      := notes
KIND     := apps
SRCS     := notes.c
CFLAGS   += -Werror

include $(firstword $(wildcard $(RITTEROS)/sdk/ritteros.mk /system/sdk/ritteros.mk) $(RITTEROS)/sdk/ritteros.mk)
