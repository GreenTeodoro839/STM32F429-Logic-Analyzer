# LA-F429 / LA-F103：STM32 逻辑分析仪（PulseView / sigrok）

8 通道逻辑分析仪固件，两块板子共用一套协议层，上位机直接用 **PulseView / sigrok**，CMake + CMSIS + TinyUSB，不需要 CubeMX。
一个 USB 虚拟串口同时支持两种工作模式，由 PulseView 选择的驱动自动切换：

| | **LA-F429**（STM32F429IGT6 开发板） | LA-F103（STM32F103C8T6 最小系统板） |
|---|---|---|
| 快照模式（`ols` 驱动） | **20 MHz，最多 33.5 M 样本**（32 MB SDRAM），设备端触发 | 4 MHz，16128 样本 |
| 流式模式（`raspberrypi-pico` 驱动） | 20 MHz；≤ 32 M 样本时先存后传，任意信号都不丢 | 1–4 通道 4 MHz，8 通道 3 MHz |
| USB 吞吐（流式） | 约 820 kB/s | 约 620 kB/s |
| 显示 | 1.3" 240×240 彩色 IPS：状态、实时电平、**上次采集的波形预览** | 0.96" OLED：状态、实时电平 |
| 采样完整性 | 每次采集统计 DMA 丢失的样本数，有丢失就报警 | — |

```
cmake -S . -B build/f429 -G Ninja                  # 默认 F429
cmake -S . -B build/f103 -G Ninja -DLA_BOARD=F103
```

---

## LA-F429

### 引脚（全部在 JD1 / JD2 排母上）

| 通道 | 引脚 | 排针 | | 通道 | 引脚 | 排针 |
|---|---|---|---|---|---|---|
| CH0（PulseView pico 驱动里叫 D2） | PI0 | JD2-15 | | CH4（D6） | PI4 | JD1-13 |
| CH1（D3） | PI1 | JD2-9 | | CH5（D7） | PI5 | JD1-3 |
| CH2（D4） | PI2 | JD2-11 | | CH6（D8） | PI6 | JD1-14 |
| CH3（D5） | PI3 | JD2-13 | | CH7（D9） | PI7 | JD1-15 |

- GND：JD1-2、JD2-3、JD2-23、JD2-24，必须与被测板共地。
- 输入带约 40 kΩ 内部下拉，悬空读 0。**只接 3.3 V 逻辑**（开了内部上下拉的引脚不耐 5 V）。
- USB：板子的 **USB2** 口（F429 自己的 OTG_FS，PA11/PA12），不是 DAP-Link 那个 USB1 口。USB1 用来供电和烧录。

### 内置测试信号（都在 JD2，和输入相邻）

| 引脚 | 排针 | 信号 | 建议接到 |
|---|---|---|---|
| PB0 | JD2-10 | 100 kHz 方波 | JD2-9（CH1） |
| PB1 | JD2-12 | 1 MHz 方波 | JD2-11（CH2） |
| PA5 | JD2-16 | 1 kHz 方波 | JD2-15（CH0） |
| PA9 | JD2-7 | UART 115200 8N1，每 10 ms 一行 `LA-F429 test 00042` | JD2-13（CH3） |

测试信号与采样时钟同源：20 MHz 下 1 MHz 信号每个周期应正好 20 个样本，可以直接用来检查有没有丢样本。

### LCD 和按键

- **KEY1** 切换页面：主页 → 引脚表 → 信息页（固件版本、HSE / SDRAM 状态、上电时测得的 DMA 采样极限）。
- 主页：状态（Ready / Wait trigger / Capturing / Uploading / Done / Streaming…）、采样率与深度、触发条件，
  8 行通道，每行左边是实时电平（实心 = 高，空心 = 低，条纹 = 在跳变），右边是**上次快照采集的波形**，
  琥珀色虚线是触发点。
- **KEY2** 在主页上围绕触发点放大波形（×4 一档，直到 1 像素 1 个样本后回到 ×1），底栏显示当前视窗时长。
- LED1 心跳（采集中快闪），LED2 = USB 已连接。

屏幕方向由 `f429/lcd.h` 的 `LCD_MADCTL` / `LCD_ROW_OFFSET` 决定。厂家例程的 `0xC0` + 行偏移 80 在这块板上是倒的，
现在用 `0x00` + 0。

### 编译与烧录

依赖：`arm-none-eabi-gcc`、`cmake`（≥ 3.20）、`ninja`、`openocd`。

```sh
git submodule update --init                # CMSIS_6、cmsis_device_f1/f4、TinyUSB
cmake -S . -B build/f429 -G Ninja
cmake --build build/f429                   # build/f429/la_f429.{elf,bin,hex}
cmake --build build/f429 --target flash    # 经板载 DAP-Link（CMSIS-DAP，c251:f001）
```

同时插着别的 CMSIS-DAP 探头时，OpenOCD 按 `F429_DAP_VID_PID`（默认 `0xc251 0xf001`）选板载那个。

udev 规则（普通用户访问、阻止 ModemManager、固定设备名 `/dev/la-f429` / `/dev/la-f103`，两块板按产品名区分）：

```sh
sudo cp tools/udev/*.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
```

### 快照模式（设备端触发，深度大）

PulseView → 驱动 **Openbench Logic Sniffer & SUMP compatibles**，串口 `/dev/la-f429`。

- 采样率 ≤ 20 MHz。PulseView 列表里 20 MHz 及以下的 1-2-5 档全部精确（120 MHz 定时器）。
- 触发：通道设 **1 = 上升沿、0 = 下降沿**；多通道 = 进入该电平组合的瞬间。触发点精确落在 PulseView 的触发标记上，
  预触发比例由 capture ratio 决定。
- ≤ 61440 样本存片上 SRAM，更多存 SDRAM。
- **上传只有约 300 kB/s**：libsigrok 的 OLS 驱动每次只从串口读 1 个字节，这是主机端的上限。
  3000 万样本要传近 2 分钟；大深度采集用流式模式（下面）更快。
- 设备选项里的 RLE 保持关闭。

```sh
sigrok-cli -d ols:conn=/dev/la-f429 --config samplerate=20M:captureratio=50 --samples 1000000 -t 0=1 -o cap.sr
sigrok-cli -d ols:conn=/dev/la-f429 --config samplerate=1M --samples 100000 -P uart:rx=3:baudrate=115200 -A uart=rx-data
```

### 流式模式（raspberrypi-pico 驱动，需要 nightly 版 PulseView / sigrok-cli）

驱动 **RaspberryPI PICO**，串口 `/dev/la-f429`。通道叫 D2–D9，对应 CH0–CH7。

- 采样率：驱动列表里 ≤ 20 MHz 的档位都精确；30 MHz 以上设备会拒绝（日志里有 `LA-F429: max 20MHz`）。
- **样本数 ≤ 3200 万时先存后传**：设备采满就停止采样，再按 USB 速度慢慢发，所以 20 MHz、8 通道、信号再密也不丢。
- 更长的连续采集要实时跟上：数据经游程编码，USB 约 820 kB/s，编码器约 15–20 MS/s（视信号而定）。实测可持续：
  1 通道 1 kHz @ 20 MHz、2 通道（1 kHz + 100 kHz）@ 15 MHz、8 通道全部测试信号 @ 1 MHz。
  跟不上时设备中止（PulseView 报错，LCD 显示 `Overflow`）。
- 触发在主机端（libsigrok 软件触发），见下面 FAQ 第 3 条。

```sh
tools/stream_bench.py /dev/la-f429 --rate 20000000 --samples 20000000 --channels 8
```

### 实测（本板）

| 项目 | 结果 |
|---|---|
| DMA 采样极限（上电自检，65536 样本，CPU 同时读缓冲） | SRAM 24 MHz、SDRAM 20 MHz 零丢失；30 MHz 起每 3 个丢 1 个 |
| 20 MHz × 1M / 4M 快照（SDRAM） | 丢失 0；1 MHz 信号的周期全部为 20 个样本（个别 19/21 成对出现：边沿恰在采样时刻上的 ±1 抖动） |
| 触发位置（SRAM 4 万、SDRAM 100 万、400 万；上升 / 下降沿；sigrok 0.5.2 与 nightly） | 边沿全部恰在触发标记上 |
| 流式 600 万样本 @ 10 MHz | 60 万个 1 MHz 周期全部为 10 个样本 |
| UART 解码（CH3） | 正确 |

---

## LA-F103

引脚 PA0–PA7 = CH0–CH7，OLED 在 PB6/PB7（I2C，SH1106/SSD1306），LED PC13，烧录用 WCH-Link（CMSIS-DAP）。
测试信号：PB0 100 kHz、PB8 1 kHz、PA9 UART；自检接 PB0→PA0、PA9→PA1。

```sh
cmake -S . -B build/f103 -G Ninja -DLA_BOARD=F103 && cmake --build build/f103 --target flash
```

快照模式 ≤ 4 MHz（4 MHz / n 档精确，旧版驱动列到 200 MHz 时 OLED 提示 `Rate not exact!`），16128 样本；
流式模式 ≤ 4 MHz（须整除 72 MHz）。

---

## FAQ：sigrok / PulseView 的已知怪癖（F103 实测问题的结论）

这些大多在主机端，固件改不了，但都有办法绕开。

1. **“只存了 4,194,304 个点”**：`.sr` 文件里逻辑数据按 4 MiB 分块（`logic-1-1`、`logic-1-2`…），
   每块正好 2²² 个 8 通道样本。读文件的脚本要把所有 `logic-1-*` 按序拼起来；PulseView 读 `.sr` 没问题。
   （实测 600 万样本：`logic-1-1` 4194304 + `logic-1-2` 1805696 = 6000000。）
2. **`samplerate=1.2M` 变成 5 kHz**：sigrok-cli 不认小数前缀，`1.2M` 被当成 1 Hz，pico 驱动再改成最低 5 kHz。
   写成 `1200k`、`2400k`、`1500k` 即可。PulseView 的下拉菜单不受影响。
3. **pico 驱动的触发位置不固定**：触发由 libsigrok 在主机端做——开始传输后第一个满足条件的边沿就是触发点，
   前面保留的样本数 = min(captureratio × 样本数, 开始传输到该边沿之间收到的样本数)。所以周期信号的触发点
   常在几百个样本处，而且触发前是低电平（上升沿，极性没有反）。`-O binary` 不带触发标记，用 PulseView 或 `.sr` 看。
   **需要固定预触发长度时用 OLS 驱动**（设备端触发，触发点恰在标记上）。
4. **pico 驱动的通道必须从 D2 起连续开**：驱动自己检查（`Digital channel mask … not continous`）。
   把要用的信号接到 CH0 起的连续通道上。
5. **报错只有 “Could not start … acquisition”**：pico 驱动把设备的说明只写进日志（sigrok-cli 的 stderr 能看到
   `sample_rate error string …`）；失败后驱动清理有 bug（`Cannot remove non-existing event source`），重开一次即可。
6. **`--scan` 不带 `conn=` 找不到设备**：两个驱动都必须指定串口（`conn=/dev/la-f429`，Windows 上 `conn=COMx`）。
7. **通道名 D2–D9**：pico 驱动按 Pico 的 GPIO 编号起名，D2–D9 = CH0–CH7，可在 PulseView 里改名。
   D9 = CH7（F429 的 PI7 / JD1-15，F103 的 PA7）；没接信号时一直是 0。
8. **没有 `--continuous`**：pico 驱动不声明连续模式，单次最多 10⁹ 样本。
9. **captureratio**：OLS 下是预触发比例（设备执行）；pico 下只影响主机软件触发的预触发缓冲，见第 3 条。

---

## 实现概要

- **公共协议层**（`common/`）：SUMP/OLS（`sump.c`）、sigrok-pico 流式与游程编码（`stream.c`）、USB 描述符。
  SUMP 命令是二进制，sigrok-pico 是可打印 ASCII 行，按字节自动分流。板子差异在各自的 `la_config.h`。
- **F429 采样**：TIM8（120 MHz）更新事件触发 DMA2 Stream1，按字节读 `GPIOI->IDR`；DMA FIFO 半满阈值、
  每 8 个样本一次 4 拍半字突发写内存（满阈值 + 16 字节突发在 24 MHz 下每 16 个样本丢 1 个）。
  双缓冲模式按 32 KiB 块走环形缓冲，块完成中断把空出的地址寄存器指向下下块。
- **SDRAM 分 bank 交错**：逻辑上连续的块依次落在 4 个 bank（W9825G6KH，FMC 60 MHz）。实测 CPU 在 DMA 正在写的 bank
  里读要 24 周期/字、另一个 bank 只要 14.5，长读突发还会饿死 DMA；高采样率时流式编码器只读已写完的块。
- **总线竞争**：GPIO 和所有外设寄存器都在 AHB1 上，20 MHz 时 DMA 每个样本只有约 1 个周期余量。
  采集中不轮询 DMA 寄存器——写入位置由 DWT 周期计数推算，每个块完成时校准；数据、栈都放在 CCM（CPU 独占）。
- **丢样检测**：停止时用 DWT 时间算出定时器应产生的请求数，与 DMA 实际传输数相减。
- **USB**：USB2 的 D+ 上有固定 1.5k 上拉（R3），复位时主机察觉不到断开，所以上电时先把 PA12 拉低到 USB 初始化为止。
- **LCD**：ST7789VW，SPI5 模式 3、15 MHz、16 位帧直接发 RGB565；帧缓冲在 SRAM1，按脏行 DMA 刷新。
  采样期间不刷屏（LCD DMA 也在 DMA2 上）。字体由 `tools/gen_font.py` 从 DejaVu Sans Mono 生成 4 bit 抗锯齿点阵。

| 目录 / 文件 | 内容 |
|---|---|
| `common/` | 协议层（两块板共用） |
| `f429/capture.c` | 定时器 + DMA 采样、SDRAM 交错、触发扫描、丢样检测、预览数据 |
| `f429/selftest.c` | 上电 DMA 采样极限测试；`sdram.c` SDRAM 初始化与自检 |
| `f429/ui.c`、`gfx.c`、`lcd.c`、`font.c` | LCD 界面、绘图、ST7789 驱动、字体 |
| `f429/testsig.c` | 测试信号 |
| `f103/` | F103 版（OLED 界面等） |
| `tools/` | `stream_bench.py` 吞吐测试、`gen_font.py`、udev 规则 |
