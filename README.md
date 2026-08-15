# OneKVM GPIO plugins

This repository contains system GPIO capability providers for OneKVM. The
`gpio-atx` service owns front-panel power/reset outputs and PWR/HDD LED inputs
through the Linux GPIO character-device v2 API.

The machine package selects a read-only default configuration at
`/run/onekvm/gpio-atx-default.json`. Users may keep that default or store an
override through the plugin settings API; overrides live in
`/etc/onekvm/plugins/gpio-atx.json`.
