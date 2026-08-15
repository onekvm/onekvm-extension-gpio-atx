# onekvm-extension-gpio-atx

[English](README.md)

本仓库包含 OneKVM GPIO ATX 扩展。扩展内部的
`onekvm-device-gpio-atx` provider 通过 Linux GPIO character-device v2 API
独占管理机箱电源键、重启键以及 PWR/HDD 指示灯输入。

provider 是一个小型 ISO C17 守护进程，使用 `sd-bus`、`json-c` 和 GPIO
cdev v2。它通过单个 `poll()` 循环处理 D-Bus 与 GPIO 边沿中断，不会周期性
轮询 GPIO；LED 变化最多每秒合并通知 20 次。

machine 软件包会在 `/run/onekvm/gpio-atx-default.json` 提供只读默认配置。
用户可以直接使用默认值，也可以通过扩展设置界面保存覆盖配置；覆盖配置位于
`/etc/onekvm/plugins/gpio-atx.json`。

构建命令：

```sh
make
```
