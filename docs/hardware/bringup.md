# 物理台架、启动调试与问题排查

状态：**NOT_EXECUTED**。本仓库包含的是规程与验证器，而不是已制造的板卡
或经签署的实物结果。

## 必备 HIL 台架

- 隔离/限流的 0-60 V 电源，额定值满足目标负载；
- 两台 DMM、四通道示波器、差分探头、电流探头以及
  带有效校准记录的逻辑分析仪；
- CAN-FD 分析仪、两个受控的 120 Ohm 终端、已知良好的线束；
- 双通道急停固定装置、受防护的负载或已禁用的电机驱动器固定装置；
- 热像仪或粘贴式热电偶、绝缘垫、PPE、防火
  隔离区域，以及安全测试所需的第二人；
- Linux 主机，具备目标 CAN 接口、摄像头、配置哈希与
  同步的 UTC 时钟。

上电之前，必须先确认板卡、线束、仪器、校准基准、操作员、评审员
与原始抓取目录的身份信息。

## 软件预检

预检仅观测前置条件；它不发送任何 CAN 或运动指令：

```bash
python3 tools/scripts/hardware_preflight.py \
  --can-interface can0 --camera-device /dev/video0 \
  --output runs/hardware/preflight.json
```

`not_ready` 即停止。只有在指定操作员于断电状态下实地验证两条通道
与安全输出之后，才能创建急停标记。

## 分阶段启动调试

1. 记录板卡序列号/版本、BOM/PCB 哈希、线束 ID、固件/配置
   SHA-256、操作员、仪器、校准记录、环境条件与照片。发现可见损坏或
   版本不匹配时立即开立缺陷。
2. 在电机电源禁用的条件下执行 `hardware/pcb/fabrication/bringup-test-plan.csv`
   的步骤 1-6。遇到限流、冒烟、过热、电源轨顺序错误、
   纹波过大、隔离失效或意外使能时立即停止。
3. 执行 J10/U8/J11 真值表，包括每条断开的通道与通道不一致情形。
   1 ms 目标需要未经剪辑的逻辑分析仪抓取。
4. 运行 CAN classic/FD 环回，然后逐项测试已贴装的 J4 接口。保留原始
   帧与错误计数器；仅有截图是不够的。
5. 运行 30 分钟额定负载热测试步骤。只有经 QA 与安全 Owner 评审后，
   才能继续首批与 48 小时规程。
6. 运行 `hardware/validation/fault-scenarios.csv` 中全部 20 行。已停止或
   失败的场景保持 `FAIL`/`HOLD`；不得删除后再作为新通过项重跑。

## 登记证据

先在 `hardware/validation/first-batch-acceptance.csv` 中为受测单元分配
真实的硬件版本与配置哈希，然后运行：

```bash
python3 hardware/validation/tools/register_evidence.py \
  --evidence-id EVT-VAL5-01-001 --scenario-id VAL5-01 --unit-id UNIT-001 \
  --operator OPERATOR --reviewer REVIEWER --captured-at 2026-08-18T08:00:00Z \
  --evidence-kind physical --instrument-ref CAN-SCOPE-01 \
  --calibration-ref CAL-2026-001 --raw-file runs/hardware/val5-01.log \
  --result PASS

python3 hardware/validation/tools/validate_validation.py
python3 hardware/release/tools/check_release_readiness.py
```

验证器会重新哈希原始文件并推导场景状态。编辑 CSV
汇总无法造出通过结果。

## 调试决策树

- **无输入/触发限流：**断电；检查 J1/F1/U1、极性以及
  VBAT 对地电阻。切勿换成额定值更大的熔断器。
- **电源轨缺失/错误：**断开 J2/J3；从 TP1 到 TP5 向下游排查。
  发现隔离失效或热插拔循环不稳定时立即隔离。
- **CAN 离线/出错：**在改动代码之前，先核对 5V/GND CAN 隔离、CANH/CANL 极性、
  恰好两个终端、位时序与屏蔽策略。
- **J4 接口故障：**对比连接器方向与冻结的引脚复用；
  抓取复位、电压与逻辑电平。不得将安全引脚改作他用。
- **意外使能/急停失效：**断电、立即隔离、
  附上真值表抓取，并上报安全 Owner。
- **热故障：**停止负载，保留电流/温度曲线，检查
  风道与接口，在处置结论签署之前不得重启。

每发现一次故障，都要在 `hardware/qa/defect-tracker.csv` 中新增一行，包含
唯一 ID、序列化单元/批次、遏制措施、证据与 Owner。根因/纠正措施
在验证之前保持留空；只有在附有复测证据时才能关闭。设备缺失或
板卡未制造属于项目阻塞项，而不是虚构的产品缺陷。
