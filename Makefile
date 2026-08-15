CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd json-c)
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
LDLIBS += $(shell $(PKG_CONFIG) --libs libsystemd json-c)

.PHONY: all clean

all: onekvm-device-gpio-atx

onekvm-device-gpio-atx: src/main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	$(RM) onekvm-device-gpio-atx
