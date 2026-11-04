# LA-F103：基于 STM32F103C8T6 的 8 通道逻辑分析仪

用一块 STM32F103C8T6 最小系统板做的 8 通道逻辑分析仪，上位机直接用 **PulseView / sigrok**。
一个 USB 虚拟串口同时支持两种工作模式，由 PulseView 选择的驱动自动切换：

| | 快照模式 | 流式模式 |
|---|---|---|
| sigrok 驱动 | `ols`（Openbench Logic Sniffer & SUMP compatibles） | `raspberrypi-pico`（RaspberryPI PICO） |
| 最高采样率 | 4 MHz | 1–4 通道 4 MHz，8 通道 3 MHz（实测，见下文） |
| 采集长度 | 16128 个样本（片上 RAM） | 不限（边采边传，受主机内存限制） |
| 触发 | 设备端，上升沿 / 下降沿 / 多通道组合 | PulseView 软件触发，支持全部边沿类型 |
| PulseView 版本 | 0.4.2（Ubuntu 自带）或 nightly | nightly（2023 年后的 libsigrok） |

另外板载 0.96" I2C OLED 显示状态和 8 路实时电平（相当于逻辑笔），并内置测试信号用于自检。

## 硬件

### 引脚

| 功能 | 引脚 | 说明 |
|---|---|---|
| **CH0–CH7** | **PA0–PA7** | 内部约 40 kΩ 下拉。**仅限 3.3 V 信号，这几个脚不耐 5 V** |
| GND | 任意 GND | 必须与被测板共地 |
| USB | PA11 / PA12 | 板载 USB 口：供电 + 数据 |
| SWD | PA13 / PA14 | 烧录调试（WCH-Link，CMSIS-DAP 模式） |
| OLED | PB6 (SCL) / PB7 (SDA) | 128×64 I2C OLED，地址 0x3C（SH1106 / SSD1306 均可） |
| LED | PC13 | 空闲时心跳闪烁，采集中快闪 |

### 内置测试信号

| 引脚 | 信号 |
|---|---|
| PB0 | 100 kHz 方波（TIM3） |
| PB8 | 1 kHz 方波（TIM4） |
| PA9 | UART 115200 8N1，每 10 ms 一行 `LA-F103 test 00042\r\n`（计数递增） |

自检：杜邦线 **PB0→PA0、PA9→PA1**，CH0 应测得精确 100 kHz，CH1 用 UART 解码器可看到递增的文本。

### OLED 显示

```
 LA-F103     USB OK      标题 / USB 状态
Idle                     状态：等待触发 / 采集中 / 上传中 / 流式中 / 完成
2MHz 16000 smp           采样率与样本数（流式模式显示 "1MHz stream 8ch"）
Trig RxFxxxxx            触发：R 上升沿，F 下降沿，x 不关心
 0  1  2  3  4  5  6  7
[] [] [] [] [] [] [] []  实时电平：空心=低，实心=高，网格=在跳变
PB0:100k PB8:1k PA9:U    提示 / 警告
```

## 编译与烧录

依赖：`arm-none-eabi-gcc`、`cmake`（≥ 3.20）、`ninja`、`openocd`。不需要 CubeMX。

```sh
git submodule update --init          # CMSIS_6、cmsis_device_f1、TinyUSB
cmake -S . -B build -G Ninja
cmake --build build                  # 生成 build/la_f103.{elf,bin,hex}
cmake --build build --target flash   # 经 WCH-Link（CMSIS-DAP）用 OpenOCD 烧录
```

Linux 下首次使用需安装 udev 规则（普通用户访问 WCH-Link 和分析仪串口，阻止 ModemManager 探测，并建立固定设备名 `/dev/la-f103`）：

```sh
sudo cp tools/udev/*.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
```

## 使用

### 快照模式（高速、设备端触发）

1. PulseView → 连接设备 → 驱动 **Openbench Logic Sniffer & SUMP compatibles**，串口 `/dev/la-f103`，扫描。
2. 采样率 **≤ 4 MHz**。时间轴精确的档位是 4 MHz / n：PulseView 中 2 MHz 及以下的 1-2-5 档全部精确。
   旧版驱动会列出到 200 MHz，超过 4 MHz 时固件按 4 MHz 采样、时间轴不准，OLED 提示 `Rate not exact!`。
3. 触发：通道设 **1 = 上升沿、0 = 下降沿**；多通道 = 进入该电平组合的瞬间。
4. 设备选项里的 **RLE 保持关闭**（不支持；打开时 CH7 被强制为 0）。

```sh
sigrok-cli -d ols:conn=/dev/la-f103 --config samplerate=1M --samples 16000 \
           -P uart:rx=1:baudrate=115200 -A uart=rx-data
sigrok-cli -d ols:conn=/dev/la-f103 --config samplerate=2M:captureratio=10 --samples 4000 -t 1=0 -O vcd -o cap.vcd
```

### 流式模式（长时间连续采集）

需要 nightly 版 PulseView / sigrok-cli（`https://sigrok.org/download/binary/pulseview/` 的 `*-NIGHTLY-x86_64-release.AppImage`）。

1. 驱动选 **RaspberryPI PICO**，串口 `/dev/la-f103`。
2. 通道显示为 **D2–D9，对应 CH0–CH7**（驱动按 Pico 引脚命名，可在 PulseView 中改名）。
3. **启用的通道必须从 D2 开始连续**（驱动要求）。只开 1–4 个通道时使用更紧凑的编码，速度更高。
4. 采样率选 ≤ 4 MHz（须整除 72 MHz，列表中 ≤ 4 MHz 的档位都满足）。

数据经游程编码后边采边传，USB 实际吞吐约 620 kB/s。信号不变时几乎不占带宽，翻转越频繁越吃带宽：
8 通道下每次跳变约 3 字节，1–4 通道约 1–2 字节。超出时设备中止本次采集（PulseView 报错，OLED 显示
`Stream OVERFLOW`），此时减少通道或降低采样率。

实测（CH0 = 100 kHz 方波 + CH1 = UART）：1–4 通道 4 MHz、8 通道 3 MHz 可持续采集；8 通道 4 MHz 溢出。
可用 `tools/stream_bench.py` 测试吞吐：

```sh
tools/stream_bench.py /dev/la-f103 --rate 2000000 --samples 4000000 --channels 4
```

## 实现概要

- **采样**：TIM1 更新事件触发 DMA1 通道 5，把 `GPIOA->IDR` 低字节写入 16 KB 环形缓冲。
  系统时钟 72 MHz（USB 需要 48 MHz，只能是 72 或 48 MHz）。
- **快照触发**：CPU 追在 DMA 写指针后面扫描（单级条件一次比较 4 个样本），找到边沿后再采完后触发样本即停，
  按 OLS 协议倒序上传。
- **流式**：同一环形缓冲，CPU 实时游程编码后经 TinyUSB CDC 发送；DMA 绕圈由传输完成中断计数，
  每编码 512 个样本检查一次是否被 DMA 追上。
- **协议分流**：SUMP 命令是二进制（0x00–0x13、0x80–0xFF），sigrok-pico 是可打印 ASCII 行，按字节自动分流。

| 文件 | 内容 |
|---|---|
| `src/capture.c` | 定时器 + DMA 采样、触发扫描、采样率换算 |
| `src/sump.c` | SUMP / OLS 协议（快照模式） |
| `src/stream.c` | sigrok-pico 协议与游程编码（流式模式） |
| `src/ui.c`、`src/oled.c` | OLED 界面与 SH1106/SSD1306 驱动（无帧缓冲，逐页刷新） |
| `src/testsig.c` | 测试信号 |
| `src/main.c` | 初始化、USB 胶合、协议分流 |

### 与 libsigrok 相关的兼容处理

- **OLS 采样率** 由主机以 100 MHz / (n+1) 的分频值下发，只有同时是 72 MHz 整数分频的档位才精确（即 4 MHz / m）。
- **libsigrok ≤ 0.5.2**（PulseView 0.4.2）只设一个触发条件时会多发一个恒成立的空触发级，固件会忽略末尾的空级，
  使新旧驱动的触发标记都落在真实边沿上。
- OLS 驱动把触发标记放在 `pre - 级数` 处，固件相应平移采集窗口。
- raspberrypi-pico 驱动只在终止字符不是一次读取的最后一个字节时才能识别它，所以溢出时连发多个 `!`。

## 后续

最初的目标平台是 STM32F429 开发板（32 MB SDRAM，更大存储深度），F103 版作为第一步。
协议层（`sump.c`、`stream.c`）与硬件无关，可直接复用。
