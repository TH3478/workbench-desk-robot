# docs/hardware

硬件启动调试与集成指南。

- [硬件接线](wiring.md)：受控的连接器映射与安全的连接顺序。
- [物理启动调试](bringup.md)：HIL（硬件在环）台架、分阶段上电、证据、调试与缺陷。
- [HW1 UR5e 提取](hw1-ur5e-extraction.md)：来源与配置说明。

总原则：所有启动调试步骤尽可能脚本化。若某一步需要人工操作（物理接线、
螺丝刀），则须记录照片参照与可验证的结果（例如「电压读数 X」）。

## 预检

仅软件的预检绝不会发送 CAN、电机或急停指令：

```bash
python tools/scripts/hardware_preflight.py --output runs/hardware/preflight.json
```

在 Linux、已配置的 CAN 接口、摄像头设备以及由操作员创建的急停标记全部就绪
之前，它会报告 `not_ready`。`not_ready` 结果表示明确阻止，而不是请求继续
使用模拟硬件。
