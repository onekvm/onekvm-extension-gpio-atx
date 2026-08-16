CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd json-c)
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
LDLIBS += $(shell $(PKG_CONFIG) --libs libsystemd json-c)
INSTALL ?= install
PREFIX ?= /usr
LIBEXECDIR ?= $(PREFIX)/libexec/onekvm
SYSTEMD_UNIT_DIR ?= $(PREFIX)/lib/systemd/system
DBUS_CONFIG_DIR ?= $(PREFIX)/share/dbus-1/system.d
DBUS_SERVICE_DIR ?= $(PREFIX)/share/dbus-1/system-services
ONEKVM_DATA_DIR ?= $(PREFIX)/share/onekvm
VERSION ?= 0.1.0

.PHONY: all clean install

all: onekvm-device-gpio-atx

onekvm-device-gpio-atx: src/main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: onekvm-device-gpio-atx
	$(INSTALL) -Dm0755 onekvm-device-gpio-atx \
		$(DESTDIR)$(LIBEXECDIR)/onekvm-device-gpio-atx
	$(INSTALL) -Dm0644 packaging/systemd/onekvm-device-gpio-atx.service \
		$(DESTDIR)$(SYSTEMD_UNIT_DIR)/onekvm-device-gpio-atx.service
	$(INSTALL) -Dm0644 packaging/dbus/org.onekvm.Device.GPIO1.conf \
		$(DESTDIR)$(DBUS_CONFIG_DIR)/org.onekvm.Device.GPIO1.conf
	$(INSTALL) -Dm0644 packaging/dbus/org.onekvm.Device.GPIO1.service \
		$(DESTDIR)$(DBUS_SERVICE_DIR)/org.onekvm.Device.GPIO1.service
	$(INSTALL) -Dm0644 packaging/onekvm/gpio-atx-device-backend.json \
		$(DESTDIR)$(ONEKVM_DATA_DIR)/device-backends/gpio-atx.json
	$(INSTALL) -Dm0644 packaging/onekvm/gpio-atx-system-plugin.json \
		$(DESTDIR)$(ONEKVM_DATA_DIR)/system-plugins/gpio-atx.json
	$(INSTALL) -Dm0644 packaging/onekvm/gpio-atx-page.json \
		$(DESTDIR)$(ONEKVM_DATA_DIR)/system-plugins/gpio-atx/$(VERSION)/web/page.json

clean:
	$(RM) onekvm-device-gpio-atx
