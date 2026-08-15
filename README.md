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

Build it with:

```sh
make
```
