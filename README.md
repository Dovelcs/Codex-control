# STM32G0B1CBT6 control firmware

用于 STM32G0B1CBT6 调试控制板的 UART 透传、外设控制和掉电保持日志固件。

## 硬件配置

- MCU：STM32G0B1CBT6，128 KB Flash，144 KB SRAM
- 系统时钟：优先使用外部 8 MHz HSE，经 PLL 输出 64 MHz；HSE 启动失败时回退到 HSI16 + PLL
- Debug UART：USART1，PA9/PA10，`1500000 8N1`，无硬件流控
- UART2：USART2，PA2/PA3，默认 `115200 8N1`，可配置范围 `1200..2000000`
- 外部 Flash：GD25Q128，SPI1 PA4-PA7，16 MB
- 控制输出：PB0 Relay、PB1 SoC Power、PB2 Reset、PB10 Loader、PB11 MaskROM
- 快捷按键：SW3 / PA0，切换继电器电源

UART2 外部接口电平必须与目标板一致。当前硬件在 3.3 V、Debug UART 1.5 Mbps 下完成了 768,000 字节零丢失环回测试；CH340C 在 2 Mbps 下存在间歇性丢字节，因此正式固件固定为 1.5 Mbps。

## 使用方式

固件上电默认进入双向透传模式：

- UART2 -> Debug UART
- Debug UART -> UART2

在 500 ms 内连续发送两次 `Ctrl+]`（字节 `0x1D 0x1D`）进入 CLI。输入 `exit`、`Ctrl+D` 或 `mode passthrough` 返回透传模式。

CLI 支持中文帮助、Tab 命令列表与补全、上下方向键历史和以下主要命令：

```text
help
status
uart2 baud get
uart2 baud set <baud>
uart2 baud save
cache status
cache dump [length]
cache dump text [length]
cache dump hex [length]
cache dump raw [length]
cache flush
cache clear
cache timestamp on|off|get
time get
time set iso <YYYY-MM-DDTHH:MM:SS.mmm+HH:MM>
time set unix <unix_ms> <+HH:MM>
time clear
relay on|off
soc power on|off
reset assert|release
reset pulse <ms>
loader on|off
maskrom on|off
pins status
exit
```

## 日志缓存

- 同时记录 UART2 RX、Debug UART TX、控制事件和校时事件
- 每条记录包含会话编号、64 位上电毫秒、方向/类型和原始负载
- 先写入 32 KB RAM，累计 1024 个 UART 数据字节或等待 60 秒后提交
- 提交到 GD25Q128 时统一使用 LZ4 压缩，Flash 格式版本为 v3
- 外部 Flash 使用可恢复的提交头、CRC、环形覆盖和预擦除备用扇区
- `cache dump` 默认输出带时间和方向的可读文本；控制字符会安全转义，连续退格会折叠显示
- `cache dump text|hex|raw [length]` 可显式选择文本、十六进制或原始二进制格式
- 日志导出和 `reset pulse` 均由主循环协作式状态机处理，不使用 RTOS

## 构建

仓库已经包含本工程使用的 STM32CubeG0 HAL/CMSIS 源码，来源版本：

```text
STM32CubeG0 b1d88f9e17290d5c5328399bb02bb5ef82deb03a
```

使用 STM32CubeIDE 导入 `STM32CubeIDE` 目录中的 Existing Project，然后构建 Debug 或 Release 配置。工程不依赖本机绝对路径。

烧录使用 ST-LINK/SWD。当前硬件不支持通过 STM32 ROM UART Bootloader 下载本固件。

## 串口压力测试

安装依赖：

```powershell
python -m pip install pyserial
```

UART2 TX/RX 硬件环回后运行：

```powershell
python tools/stm32_baud_stress.py 1500000 --port COM4 --status-trials 500 --loop-trials 1000 --loop-timeout 3
```

测试结束会清空测试日志并返回透传模式。
