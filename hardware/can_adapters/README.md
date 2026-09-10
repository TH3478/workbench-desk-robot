# CAN 适配器

本目录记录物理适配器边界。Linux 参考实现是
`libs/hardware/workbench/hardware/socketcan_transport.py` 中的标准库
`SocketCANTransport`，由 `SafeCANBus` 和唯一的 `DeviceRuntime` 托管。它只打开
一个 `AF_CAN`/`CAN_RAW` 描述符，不创建适配器本地的工作线程、队列、回调注册表
或生命周期状态机。

候选物理路径仍由 Owner 把关：

- 隔离的 USB-CAN-FD 原型适配器（首条启动调试路径）；
- CANable 2.0（USB、SocketCAN）；以及
- PCAN-USB（Peak Systems），取决于其 Linux 驱动和许可证配置。

适配器必须保持冻结的 Wire V1 ID 和 payload 布局。它必须在把帧交给协议解码器
之前校验标准/扩展/RTR/错误标志、DLC、原始 ID 一致性、内核时间戳和接收队列
丢包。只有经过校验的 ACK、STOP_ACK 和遥测帧才能成为 `CanExternalRecord`
投影。被拒绝的帧仍可通过受限接收结果和诊断获取，但不会发布到外部队列。
外部 HTTP/看板消费者是只读的：它只接收不可变记录，永远拿不到 CAN fd、
debugfs 句柄或写权限。

在特权环境中用 `wbcan0` 或 `vcan0` 接口运行虚拟边界探针：

```bash
python3 kernel/wbcan/test_socketcan_ingress.py wbcan0 \
  --source virtual-wbcan \
  --report /tmp/wbcan-socketcan-ingress-report.json
```

报告明确标记为 `virtual-socketcan-ingress`。缺少权限或虚拟接口即为
`NOT_EXECUTED`；虚拟 `PASS` 不能证明物理 CAN 仲裁、MCU、执行机构、电气
完整性、PREEMPT_RT 或硬实时截止时间。

物理搭建、六域发现、时间戳、原始抓取哈希、标定参照和清理要求定义于
[`docs/hardware/can_bring_up.md`](../../docs/hardware/can_bring_up.md)。
