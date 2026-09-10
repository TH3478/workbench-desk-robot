# ADR-0003: 安全 MCU 目标为 RISC-V，在 QEMU 中验证

状态：已接受
日期：2026-08-05

## 背景

`firmware/virtual_mcu/` 是一个 Python 状态机。它是好用的契约桩——Motion 可以在任何硬件存在
之前针对它构建——但它证明不了固件的任何东西。一个 Python 类断言自己进入了 `safe_stop`，
并不能说明交叉编译的二进制在真实定时器中断下也做同样的事。

当时尚未选定 MCU 部件。选型过晚意味着整个固件工作挤在最后一个月，紧邻真实硬件启动调试——
那里的进度风险已经最高。

## 决策

安全 MCU 目标为 **RISC-V（rv32imac）**。参考部件是 **CH32V307**（片上 CAN，约 ¥10，单一
用途足以胜任安全元件）。固件在 **CI 中的 QEMU** 验证，然后在 P3 上板验证。

固件分为两层：

```
firmware/mcu/
  core/           platform-independent C: state machine, frame codec,
                  watchdog, dedup, ring buffers.
                  No peripheral registers. No vendor headers.
  hal/qemu/       QEMU target, CTU CAN FD over PCI
  hal/ch32v307/   real board, CH32V307 CAN peripheral
  hal/host/       x86_64 build for fast logic tests
```

`core/` 从一份源码编译到三个目标。换板卡意味着写新的 `hal/`，而不是碰 `core/`。这与全项目
规则一致：实现可替换，契约不可替换。

## QEMU 的局限，直说

QEMU 建模 **SJA1000** 和 **CTU CAN FD**，都走 PCI。它**不**建模 CH32V307 CAN 外设。MCU
专用 CAN 模型是一次一个部件地加入 QEMU 的（STM32 bxCAN 工作是一个补丁系列，不是上游）。

因此划分是：

| QEMU 中已证明 | 需要板卡 |
|---|---|
| 状态机转换 | CH32V307 CAN 寄存器行为 |
| 真实定时器中断下的看门狗时序 | 位时序（BRP/TSEG1/TSEG2/SJW） |
| 跨序号回绕的去重 | 错误帧、bus-off 恢复 |
| 帧编解码、ID 分区强制 | 电气行为、EMI |
| 无 malloc 与 FP 指令 | 欠压、上电复位 |

这没有扩大我们的声明。README 已列出 CAN 电气行为与总线时序未经仿真证明。

## 为什么是 RISC-V

- 免版税 ISA，完全开放工具链——可复现性是项目目标
- 上游 QEMU 对 rv32 支持成熟
- 现在就能买到便宜的真实部件（CH32V307、ESP32-C6、GD32VF103）
- `core/` 不带厂商头文件，因此换部件只意味着新的 `hal/`

具体选 CH32V307 而非 ESP32-C6：这是一个执行急停与看门狗的安全元件。WiFi 与蓝牙是一个必须
与网络保持独立的部件上的攻击面。

## 被否决的替代方案

**保留 Python 模型，只在真实硬件上测试。**把所有固件风险推到 P3，与机械臂启动调试挤在一起。
故障覆盖在最后一个月之前都无法证明。

**为 CH32V307 CAN 外设写 QEMU 设备模型。**本身就是一个项目。对 v0.1 不值得。

**ARM + STM32。**QEMU bxCAN 支持是补丁系列，不是上游保证。这里没有相对 RISC-V 的优势，
而且放弃了开放工具链的论据。

## 成本

MCU 轨道从大约一周（Python 状态机）增长到 P1 中约 2.5 周：工具链、启动代码、QEMU 测试框架、
HAL 分层。P1 G-track 里程碑推迟约一周。

CI 增加一个 `mcu-qemu` 任务。与 Gazebo 相比很便宜——无 GPU、无显示、每次运行几秒钟。

## 后果

- `firmware/virtual_mcu/`（Python）在 P1 期间保留，作为 Motion 构建所依据的契约，也作为对
  C 核心做差分测试的参考模型。一旦 `core/` 通过相同故障套件，它在 **P1 结束时退役**。一个
  状态机的两个实现不能活过 P1。
- `mcu_protocol` schema 不变。同样的线上格式，底下是真实实现。
- P3 回报：同一个 `core/` 二进制在 QEMU 和板卡上通过相同故障套件，只有 `hal/` 不同。

## 工具链

上游 GCC 可用。WCH 的私有指令（`mcpy`）只在 CH584/585 上强制，CH32V307 上不是，因此不需要
MounRiver。

| 工具链 | Triplet | 用途 |
|---|---|---|
| Ubuntu `gcc-riscv64-unknown-elf` | `riscv64-unknown-elf-` | CI，若其 rv32 multilib 可用 |
| [xpack `riscv-none-elf-gcc`](https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack) | `riscv-none-elf-` | 后备与本地开发；更新、可靠的 rv32 |
| WCH MounRiver | `riscv-none-elf-` | '307 不需要 |

烧录使用 [`wlink`](https://github.com/ch32-rs/wlink)，而不是 OpenOCD：

```
wlink mode-switch --rv     # put WCH-LinkE into RV mode once
wlink flash build/board/mcu.elf
```

`wchisp flash` 走 USB ISP（BOOT0 + RESET），完全不需要适配器。OpenOCD 仅用于 GDB 调试时需要，
且需要带 `--enable-wlinke` 构建的 WCH fork。

## 硬件

**CH32V307 有 CAN 控制器但没有 CAN 收发器。**收发器是外置的，不在 EVT 板上。

| 项目 | 部件 | 数量 | 为什么 |
|---|---|---:|---|
| 板卡 | CH32V307V-EVT-R1 | 2 | 板载 WCH-Link，无需独立调试器 |
| **CAN 收发器** | SN65HVD230 模块（3.3V） | 2 | **必需。不在板上。** |
| USB-CAN 桥 | CANable 2.0 或 PCAN-USB | 1 | 让主机用 `candump` 看到真实帧 |
| 逻辑分析仪 | 任意 8 通道 | 1 | FW22 看门狗时序 |
| 双绞线 + 2×120Ω | — | — | 总线两端都加终端 |

两块板，不是一块：CAN 是总线协议。仲裁、错误帧和 bus-off 恢复（FW19）无法用单个节点验证。

专门用 3.3V 收发器：CH32V307 I/O 是 3.3V。5V TJA1050 需要电平转换，那是又一个容易出错的地方。

P2 末（任务 H1）购买。FW1-FW16 全部在 QEMU；FW17 才第一次需要板卡。有一个值得花 ¥150 的
例外：早点买一块板，在上面跑一次 FW3 状态机，以检验“QEMU 通过的代码在硬件上也通过”这个
假设。在 P1 发现这一点胜过在 P3 发现。

## 参考

- QEMU CAN 仿真：https://www.qemu.org/docs/master/system/devices/can.html
- SocketCAN vcan：https://www.kernel.org/doc/html/latest/networking/can.html
- CH32V307 SDK 与数据手册：https://github.com/openwch/ch32v307
- 开源 CH32V 工具链指南：https://github.com/cjacker/opensource-toolchain-ch32v
- Zephyr 板卡页面（引脚定义）：https://docs.zephyrproject.org/latest/boards/wch/ch32v307v_evt_r1/doc/index.html
