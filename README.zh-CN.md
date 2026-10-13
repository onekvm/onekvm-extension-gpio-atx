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

provider 与板卡型号无关，不依赖具体的 machine 软件包。GPIO 默认映射及其
启动准备顺序由 machine 包负责；NanoKVM 已通过检测服务和 Core 服务配置完成
这一集成。已有的 `use_default: false` 用户映射也能在没有机型默认配置时启动
provider。OpenEmbedded 按目标 CPU 架构打包。

NanoKVM Pro Desk 与 PCIe 共用 ATX 映射，由 `onekvm-machine-nanokvm-pro`
提供：电源键 `gpiochip0:7`、重启键 `gpiochip1:3`、PWR 灯
`gpiochip2:11`、HDD 灯 `gpiochip2:10`。按键输出高电平有效、空闲低电平；
两个灯输入均低电平有效。这些是 bank 内偏移，不是厂商 sysfs 的全局 GPIO
编号 7/35/75/74。Pro 检测服务在 provider 启动前发布默认配置。
调试构建强制重新编译，避免误安装另一 CPU 架构留下的编译产物。

构建命令：

```sh
make
DESTDIR="$PWD/stage" make install PREFIX=/usr
```

安装目标会同时部署 provider、systemd unit、D-Bus policy/activation 文件，
以及 OneKVM backend 和 system-plugin 描述文件。
