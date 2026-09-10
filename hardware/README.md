# hardware

真实硬件适配器。每个子目录都将一个物理设备封装在与仿真中相同的契约之后，
因此把 vcan 换成真实 CAN、或把模拟相机换成 USB 相机，都不会改变任何上游逻辑。

```
hardware/
  can_adapters/     SocketCAN / USB-CAN bridges (CANable, PCAN, etc.)
  cameras/          USB, RGBD and event cameras
  arm_drivers/      Vendor SDK wrappers → ros2_control hardware interface
  safety/           Hardware e-stop, watchdog, safety PLC bridge
  mechanical/       Parametric enclosure, chassis, impact and stability package
  pcb/              Controller/power PCB architecture and KiCad engineering package
  motor_driver/     Dual-axis traction childboard, motor candidates and fail-closed gates
  manufacturing/    Assembly, test, quality, rework, EHS and release process
  procurement/      Controlled BOM, quote requests, supplier review and PO gates
  qa/               Inspection standards, FMEA, AQL and compliance evidence gates
  validation/       SIM2REAL, diagnostics, fault injection and field acceptance
  release/          Cross-package release-readiness register and fail-closed gate
```

**规则**：这里的每个适配器都必须满足与其仿真对应物相同的契约。
`CanMotorAdapter` 必须发出与虚拟设备相同的 `action_result`。
验证器永远不会知道正在运行的是哪一个。

## 发布事实

工程包是可复现、可测试的，但它们并不代表已进行实物构建、供应商报价、
实验室认证或现场运行。采购、QA 和验证报告会有意让这些外部闸门保持阻塞，
直到附上带日期的证据为止。
