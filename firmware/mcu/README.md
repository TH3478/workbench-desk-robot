# 安全 MCU 固件

目标：RISC-V rv32imac。参考器件：CH32V307。
决策与理由：`docs/decisions/ADR-0003-mcu-riscv-qemu.md`。

## 布局

```
core/           platform-independent C. State machine, frame codec,
                HAL/Wire bridge, watchdog timing, dedup, ring buffers.
                No peripheral registers. No vendor headers.
hal/qemu/       QEMU target, CTU CAN FD over PCI. Used by CI.
hal/ch32v307/   real board, CH32V307 CAN peripheral. P3.
hal/host/       x86_64 build, for fast logic tests.
tests/          shared test suite, runs against all three targets.
```

`core/` 从单一源码编译到三个目标。换板意味着写一个新的 `hal/`，而不是动 `core/`。

## 唯一规则

`core/` 不得包含厂商或平台头文件。任何寄存器访问都属于 `hal/`。`core/` 内出现 `#ifdef CH32V307` 就意味着边界被违反。

## 权威边界

`core/` 下的 C 安全状态机及其共享 Host/QEMU 迁移套件是 MCU 安全行为的权威。`core/frame_codec.[ch]` 下的无分配 Wire V1 编解码器、`docs/architecture/mcu-wire-v1.md` 中的二进制契约，以及共享的 Host/QEMU 黄金向量是 Classic CAN 载荷编码的权威。`firmware/virtual_mcu/` 已退役为安全参考与奇偶校验基准。它只作为早期 Python 消费者的旧式兼容存根保留，不是 C 协议、固件或物理安全行为的证据。对该模型的改动需要单独的 Issue，且不得被静默当作 C 奇偶校验工作。

## QEMU 证明什么、不证明什么

QEMU 建模 SJA1000 与 CTU CAN FD，而不是 CH32V307 CAN 外设。

| QEMU 中已证明 | 需要板卡 |
|---|---|
| 状态机迁移 | CH32V307 CAN 寄存器行为 |
| 真实定时器中断下的看门狗时序 | 位时序（BRP/TSEG1/TSEG2/SJW） |
| 跨序列回绕的去重 | 错误帧、bus-off 恢复 |
| 帧编解码器、ID 分区强制 | 电气行为、EMI |
| 原始包络拒绝与 STOP 优先桥逻辑 | 控制器 FIFO/IRQ 与线路仲裁 |
| 无 malloc 与 FP 指令 | 欠压、上电复位 |

## 构建

```bash
make host        # x86_64 library for fast tests
make qemu        # rv32imac ELF for QEMU
make board       # rv32imac ELF to flash (P3)

make test-host   # logic tests, seconds
make test-host-sanitize  # Host corpus under ASan and UBSan
make test-qemu   # fault suite in QEMU, what CI runs
```

## 状态

平台无关的 C 安全状态机由 Issue #53 实现。Issue #54 增加严格的 Classic CAN Wire V1 编解码器与共享 Host/QEMU 黄金向量。Issue #60 增加无分配心跳看门狗、受限的 STOP 确认时序、假时钟测试与 QEMU 机器定时器/看门狗证据。Issue #61 增加定长内存的普通命令回放窗口、可信启动会话闸门与共享 Host/QEMU 回绕语料。Issue #180 增加严格的原始 HAL/Wire V1 桥与受限的 Host 假 CAN 传输。QEMU 与物理目标的 CAN 驱动、六域仲裁与物理 CAN 验证仍是单独的 Owner 门控后续工作，且为 `NOT_EXECUTED`。

## CAN HAL/Wire 边界

`core/can_bridge.[ch]` 是唯一的原始包络映射。HAL 暴露 11 位 `arbitration_id`、DLC、显式 extended/RTR/error/FD 标志与八个数据字节。逻辑 16 位 `command_id` 留在 Wire V1 载荷字节 1..2。只有无标志、DLC-8 且带已知 Wire V1 ID 的标准帧才能被解码；格式错误或方向错误的流量不产生状态机事件。

MCU 入口在普通命令路由之前检查 STOP。有效 STOP 绕过普通启动会话闸门与去重窗口，且其 ACK 仅在 `hal_can_send()` 接受传输交接后才确认。完整契约、假证据与物理/多节点限值记录在 `docs/architecture/mcu-can-hal-boundary-v1.md`。

## 时序安全路径

`core/watchdog.[ch]` 拥有时序状态，但不拥有定时器或看门狗寄存器。只有完整、有效且序列更新的普通帧可以刷新软件链路看门狗。格式错误、重试、重复、过期与 STOP 流量不延长执行期限。错过期限进入既有锁存的 `FAULT/watchdog_expired` 状态并发出一个遥测记录。

有效 STOP 立即把状态机迁移到 `SAFE_STOP` 并创建一个关联的 `STOP_ACK` 交接记录。传输必须在受控期限前确认交接；否则 core 发出一个本地 `STOP_TIMEOUT` 结果并绝不声称已停止确认。精确常量与时钟回绕规则记录在 `docs/architecture/mcu-watchdog-v1.md`。

STOP 交接挂起期间，相等的 `retry_count` 是精确的链路级回放，严格更大的计数是协议级重试。递减或回绕的重试计数为过期，被拒绝且不改变挂起的 ACK 关联。

## 普通命令回放保护

`core/command_dedup.[ch]` 是解码后普通命令的平台无关入口。它保留八条缓存的语义 ACK 记录，应用冻结的 15 位半程比较，在接受新序列纪元前清除回绕前记录，且绝不分配内存。精确重复与递增的协议重试回放原始结果，不再派发另一个安全事件或刷新看门狗。冲突、递减、过期与被驱逐的尝试以 `duplicate_frame` 失败即拒绝。

启动时普通命令派发是关闭的。传输必须在打开可信会话闸门之前排空排队的会话前流量；普通安全复位不擦除回放历史。STOP 留在独立看门狗路径上，不能被已满或已关闭的普通窗口消费。精确算法与证据限值记录在 `docs/architecture/mcu-command-dedup-v1.md`。
