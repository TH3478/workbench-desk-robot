# SocketCAN CAN 启动调试与 HIL 证据

状态：**仅为规程；在指定操作员记录所需证据之前，物理执行状态为
`NOT_EXECUTED`**。

本文档是 `hardware/can_adapters` 的启动调试参考。生产环境中的 Linux 边界
是受控适配器所拥有的一个 `AF_CAN`/`CAN_RAW` 描述符。外部看板与 HTTP 客户端
可以读取经校验的 `CanExternalRecord` 投影，但绝不能拿到该描述符，
也不得写入 CAN、debugfs 或安全控制路径。

## 1. 虚拟前置条件与证据

在具备特权的 `wbcan0` 或 `vcan0` 接口可用之后，运行虚拟探针：

```bash
python3 kernel/wbcan/test_socketcan_ingress.py wbcan0 \
  --source virtual-wbcan \
  --report /tmp/wbcan-socketcan-ingress-report.json
python3 kernel/wbcan/test_socketcan_ingress.py \
  --validate-report /tmp/wbcan-socketcan-ingress-report.json
```

该报告的范围限定为 `virtual-socketcan-ingress`。它记录 Linux
内核、内核配置哈希、接口、来源、精确的 ACK/遥测/重复/
无效投影与清理状态。缺少特权、CAN 网络设备或
内核能力即为 `NOT_EXECUTED`；绝不会被转换成 `PASS`。

虚拟 `PASS` 仅证明 Linux 软件路径：

```text
AF_CAN peer -> SocketCAN -> SocketCANTransport -> SafeCANBus
  -> Wire V1 validation -> bounded runtime -> CanExternalRecord
```

它并不能证明收发器、线缆、物理控制器、MCU、电机、
急停、PREEMPT_RT 调度或硬实时时限。

## 2. 主机与接口预检

在开通物理总线之前，记录以下字段：

| 字段 | 所需值/证据 |
| --- | --- |
| 板卡与 Linux 镜像 | 板卡序列号、版本、发行版、`uname -a` |
| 内核配置 | `/proc/config.gz` 或 `/boot/config-$(uname -r)` 的 SHA-256；`CONFIG_CAN`、`CONFIG_CAN_RAW`、`CONFIG_CAN_DEV` |
| 接口 | 确切的 `can0` 名称、网络命名空间、`ip -details link show can0` |
| 适配器 | 隔离 USB-CAN-FD 原型型号、序列号、驱动与固件版本 |
| 时序 | 标称/数据比特率、采样点、重启策略与主机时钟源 |
| 线束 | CANH/CANL 极性、隔离基准、两个 120 欧姆终端与线束 ID |
| 校准 | 具有有效校准记录的分析仪、示波器/探头与电流仪器 ID |
| 原始抓取 | 不可变的抓取文件路径与 SHA-256，而非仅有截图的汇总 |

第一条物理路径是隔离 USB-CAN-FD 原型适配器。本仓库
不代替电气/硬件 Owner 选择载波、比特率、收发器或厂商驱动。
若任何所需值仍为 `TBD`，则停止并记录 `NOT_EXECUTED`。

接口必须放入目标网络命名空间，并且权限必须授予指定的服务账号或组。
不要全局放宽设备权限。确认只有受控适配器打开 CAN
接收 fd；看板或 shell 诊断只有在经批准的测试计划下才可
使用单独的只读抓取 socket。

观测命令示例（这些命令不代表已就绪）：

```bash
uname -a
ip -details link show can0
ip netns identify "$(printf '%s' "$$")" || true
sha256sum /boot/config-$(uname -r)
ethtool -i can0
```

## 3. 过滤器与时间戳契约

在启用流量之前配置适配器：

- 只安装已批准的标准 Wire V1 仲裁 ID 过滤器与
  经单独评审的 CAN 错误过滤器；
- 显式保留标准帧、扩展帧、RTR 与错误标志位；
- Wire V1 要求 Classic CAN DLC 8，并拒绝 CAN-FD、截断、
  格式错误的附加数据以及相互矛盾的原始 ID；
- 启用 `SO_TIMESTAMPNS`，并保留内核时间戳、主机单调时钟
  观测、主机墙钟观测、来源、接口与入口
  序号；
- 在存在 `SO_RXQ_OVFL` 时观测它，并在外部
  记录中保留计数器；丢弃计数是丢失的证据，而不是成功
  送达的证据；
- 保持指令、遥测、健康与外部投影的容量固定，并
  记录各自的丢弃计数器。

只有完整的 ACK、STOP_ACK 与遥测帧才能跨越 Wire V1 边界。
格式错误、重复、迟到、无关联、出错以及关机后的帧均为
拒绝项，不能刷新指令或声称完成。

## 4. 六域发现与恢复

BSP 契约定义了六个控制器域：`MCU-BASE`、`ARM-L-CTRL`、
`ARM-R-CTRL`、`TOOL-L-CTRL`、`TOOL-R-CTRL` 与 `MCU-SAFETY`。现有 Wire
V1 ID 不包含节点地址，因此不得通过悄悄修改 ID 或载荷的方式，
将独立响应的域放到共享总线上。物理固定装置应使用
经 Owner 批准的仲裁/分段方案。

对每个域，抓取：

1. 全新启动/会话身份与心跳；
2. 来源/接口身份与精确的原始帧；
3. 正常遥测与 ACK/动作结果记录；
4. 重复、迟到、格式错误、链路断开与总线关闭/重启行为；
5. 执行器禁用或替换为受防护负载条件下的 STOP 与复位行为；以及
6. 评审员与安全 Owner 的处置结论。

缺少 `MCU-SAFETY` 或任何必需域都视为失败即拒绝式停止，而不是
部分成功。Linux 重启、MCU 重启与总线关闭必须清除过期的
关联状态并重新进行发现。不得根据虚拟 `wbcan` 结果推断物理状态。

## 5. 机器可读的 HIL 记录

在原始抓取旁边保存一份不可变的 JSON 记录。至少必须包含：

```json
{
  "result": "PASS | FAIL | NOT_EXECUTED",
  "board_revision": "...",
  "kernel_version": "...",
  "kernel_config_sha256": "...",
  "interface": "can0",
  "network_namespace": "...",
  "adapter": {"model": "isolated-usb-can-fd-prototype", "serial": "..."},
  "six_domains": ["MCU-BASE", "ARM-L-CTRL", "ARM-R-CTRL", "TOOL-L-CTRL", "TOOL-R-CTRL", "MCU-SAFETY"],
  "clock_source": "...",
  "nominal_bitrate": "...",
  "data_bitrate": "...",
  "calibration_refs": ["..."],
  "raw_capture": {"path": "...", "sha256": "..."},
  "operator": "...",
  "reviewer": "...",
  "captured_at_utc": "..."
}
```

`PASS` 要求每个字段均已填写、六个域全部观测到、
原始抓取哈希可重新计算、所有必需的故障/恢复检查均通过、
且已记录 Owner 评审。缺少板卡、适配器、抓取、
校准、特权或物理输入一律判为 `NOT_EXECUTED`。电气、安全或协议检查
失败保持 `FAIL`，且不得被后续重跑抹除。

## 6. 清理检查清单

每次运行结束时：

- 解除所有仅用于测试的故障控制，使接口处于文档规定的
  安全状态；
- 停止运行时，汇合其唯一的工作线程并关闭适配器 fd；
- 确认关机后没有残留的过期外部记录；
- 移除临时虚拟接口并关闭对端/抓取 socket；
- 保留原始抓取、报告、命令记录与清理结果；以及
- 将任何清理失败记录为 `FAIL`。

本规程是证据闸门，而不是实物结果。在可复现的
记录出现之前，物理 CAN、MCU、执行器、电气与硬实时
声明均保持 `NOT_EXECUTED`。
