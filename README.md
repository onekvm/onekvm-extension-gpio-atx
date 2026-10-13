# onekvm-extension-gpio-atx

[中文](README.zh-CN.md)

This repository contains the OneKVM GPIO ATX extension. Its internal
`onekvm-device-gpio-atx` provider owns front-panel power/reset outputs and
PWR/HDD LED inputs through the Linux GPIO character-device v2 API.

The provider is a small ISO C17 daemon built on `sd-bus`, `json-c`, and GPIO
cdev v2. It uses one `poll()` loop for D-Bus and GPIO edge interrupts; LED
changes are coalesced to at most 20 updates per second without periodic GPIO
polling.

The machine package selects a read-only default configuration at
`/run/onekvm/gpio-atx-default.json`. Users may keep that default or store an
override through the plugin settings API; overrides live in
`/etc/onekvm/plugins/gpio-atx.json`.

The provider is independent of the board model and does not depend on a
specific machine package. A machine package owns its GPIO defaults and orders
their preparation before this service starts; NanoKVM already does this through
its detector and Core service integration. An existing user mapping with
`use_default: false` can also start the provider without machine defaults.
OpenEmbedded packages the provider for the target CPU architecture.

NanoKVM Pro Desk and PCIe use the same ATX mapping, supplied by
`onekvm-machine-nanokvm-pro`: power `gpiochip0:7`, reset `gpiochip1:3`,
PWR LED `gpiochip2:11`, and HDD LED `gpiochip2:10`. Button outputs are
active-high and idle low; both LED inputs are active-low. These are
bank-local offsets, not the vendor sysfs global GPIO numbers 7/35/75/74.
The Pro detector publishes the defaults before this provider starts.
Debug builds force recompilation so an existing binary from another CPU
architecture cannot be installed accidentally.

Build it with:

```sh
make
DESTDIR="$PWD/stage" make install PREFIX=/usr
```

The install target deploys the provider together with its systemd unit, D-Bus
policy/activation files, and OneKVM backend and system-plugin metadata.
