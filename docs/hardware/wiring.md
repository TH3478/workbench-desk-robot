# 硬件接线

这是基线 `WB1-HW-ICD-REV-A` 的 EVT 接线视图。受控的引脚级来源是
`hardware/pcb/connector-pinout.csv`；该文件中未决的行必须先获得批准，才能
下单线束或已贴片的 PCB。

```text
36-60 V bench supply / battery
        |
       J1  (keyed input, two positive + two return contacts)
        |
   F1 -> U1 hot-swap/protection -> U2 isolated 12 V
                                      |-- J2 motor auxiliary 12 V
                                      |-- U3 -> J3 Jetson dev-kit 12 V
                                      `-- U4 -> 3V3_LOGIC

Jetson dev kit <-- J4 3.3 V control backplane --> U5 MCU
                                                   |
                         U7 isolated supply -> U6 CAN FD -> J5/J6

Current controller: Dual-channel E-stop -> J10 -> safety logic
                                              |-- J11 single-channel diagnostic output only
                                              `-- MOTOR_ENABLE_REQ from U5 is a request only

Future safety ECO: Dual-channel E-stop -> J10 -> K1/K2 -> H09 -> childboard J_SAFE

Controller J2 (H02, four pins) -> replaceable traction childboard J_PWR
Controller J5/J6 isolated CAN (H10) -> childboard J_CAN
Childboard J_ML/J_MR (H11/H12) -> external M1/M2 motor terminals
Childboard J_ENC_L/J_ENC_R (H13/H14) -> external M1/M2 encoder pins
```

## 连接器映射

| 连接器 | 引脚 | 连接 | 上电前必查项 |
|---|---:|---|---|
| J1 | 1-2 `VBAT_RAW`; 3-4 `GND_PWR` | 36-60 V 输入，10 A 熔断包络 | 两条并联的 18 AWG 供电与回流触点；键位、极性与熔断器分断额定值 |
| J2 | 1-2 `12V_ISO`; 3-4 `GND` | 电机辅助电源，120 W 总功率 / 10 A 受控包络；16 A 仅为触点承载能力 | 驱动器浪涌/回馈批准与外部支路熔断器 |
| J3 | 1-2 `JETSON_12V`; 3-4 `GND` | Jetson 开发套件直流输入 | 所购开发套件版本与极性 |
| J4 | 1/3 `3V3`; 2/4 GND; 5-20 control; pin 8 `JETSON_ENABLE_REQ` | Jetson 到 MCU 的 SPI/I2C/UART、Jetson 使能、六路 CS、复位与安全状态 | 对照详细原理图核对引脚复用与方向 |
| J5/J6 | 1 `CANH`; 2 `CANL`; 3 `GND_CAN_ISO`; 4 NC | 隔离 CAN-FD 菊花链 | 终端匹配、屏蔽策略、不得与逻辑地短路 |
| J10 | A out/return, B out/return | 双通道急停回路 | 通道相互独立；出现不一致必须禁用 |
| J11 | safe enable, E-stop sense, GND, 3V3 | 当前单通道安全输出 | 与子板 `J_SAFE` 不兼容；不得拆分或改作他用 |
| J_PWR | 1-2 `12V_MOTOR_AUX`; 3-4 `GND_MOTOR` | 子板电源输入，经 H02 与控制器 J2 一一对应 | 支路熔断器、极性、浪涌、回馈与 10 A 总电流上限 |
| J_SAFE | A enable/return, B enable/return | 未来经 H09 实现的控制器 J10/K1/K2 安全 ECO | 两条独立通道、不一致锁存、无软件旁路 |
| J_CAN | `CANH`, `CANL`, `GND_CAN_ISO`, NC | 经 H10 的隔离 CAN 指令/诊断链路 | 不得连接本地地；屏蔽/泄放端接仍为 TBD |
| J_ML/J_MR | 各两个电机端子 | 经 H11/H12 连接外部 M1/M2 牵引电机 | 候选 16 AWG Mini-Fit Jr，候选限值 5.5 A，弯曲半径 20 mm |
| J_ENC_L/J_ENC_R | 各含 VCC、GND、A、B | 经 H13/H14 连接外部 M1/M2 编码器 | 编码器电气电平与屏蔽/泄放端接仍为 TBD |

不要将 J5/J6 的引脚 3 连接到逻辑地。不要将
`MOTOR_ENABLE_REQ` 桥接到 `MOTOR_ENABLE_SAFE`；`JETSON_ENABLE_REQ` 是独立的
计算电源请求。J7-J9 是下游接口
定义，本板修订版未贴装。

所有电缆屏蔽层均为单端机箱连接。H04-H10 在
控制器电缆入口处泄放；H13/H14 在子板电缆入口处泄放。切勿
将屏蔽层或泄放线用作信号或电源回流，并须对远端做绝缘处理。

## 台架连接顺序

1. 保持电源关闭，电流限值设为 0.25 A。保持 J2/J3 断开。
2. 核对 J1 极性、VBAT 对地电阻大于 10 kOhm、机箱
   搭接、急停通道独立性与连接器键位。
3. 在施加 36 V 之前，将示波器/DMM 探头接到 TP1-TP5。
4. 给 J1 上电，验证受保护与隔离电源轨，然后断电。
5. 在 J3 连接 Jetson；只有在空载与 Jetson 阶段通过后，才在 J2 连接电机辅助负载。
6. 连接 J5/J6，总线两端各放置一个 120 Ohm 终端，且仅此两个。
7. 不要将子板 `J_SAFE` 连接到当前的 J11。只有在 J10/K1/K2 真值表测试证明两条通道
   及不一致情形均正确后，才能启用未来的安全 ECO。

验收值与所需抓取由
`hardware/pcb/fabrication/bringup-test-plan.csv` 与
`hardware/pcb/testpoint-coverage.csv` 管控。
