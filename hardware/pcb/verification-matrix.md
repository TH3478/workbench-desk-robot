# PCB 验证矩阵

| 任务 | 证据 | 状态 / 闸门 |
|---|---|---|
| PCB1 | KiCad 工程、110 符号详细原理图、已布线的 EVT 载板、ERC 报告 | ENGINEERING COMPLETE；元件与安全批准仍是下单闸门 |
| PCB2 | 电气规格、生成报告、候选 BOM、官方 U2 排除证据 | BLOCKED：U2 MPN 与焊盘图形为 TBD；DCM3623T50M31C2T00 已被排除 |
| PCB3 | 5 kVrms 隔离 CAN FD、U6 8.1 mm 与 U7 5.87 mm 全层隔离屏障、扼流圈/TVS/终端计划 | BLOCKED：U7 模组安全适用性与浪涌测试仍属外部事项 |
| PCB4 | 保险丝、反接保护、热插拔 UV/OV/浪涌、急停时序 | DESIGN COMPLETE；台架跳闸时间测试待做 |
| PCB5 | `connectors.csv`、`connector-pinout.csv`、请求/安全使能分离 | EVT INTERFACE BASELINE COMPLETE；需要 Owner 引脚复用签核 |
| PCB6 | 八层 160 x 130 mm 板卡、114 个封装、1,250 个走线/过孔条目、31 个铜区、8 个 SMT 测试焊盘、Gerber/钻孔/IPC-D-356 | CONCEPT DRC CLEAN；MPN 冻结后必须通过 ECO 替换 U2 封装/布局/布线 |
| PCB7 | 77 行分组 BOM、批准登记册与已签署的 AVL/CTO 闸门 | BLOCKED：全部 68 个受控组都需要批准；U2 仍是 TBD 需求包络 |
| PCB8 | CAN 规则、匹配的 RAW 盲孔过渡、图度量指标、参考铜区端点覆盖率、零过孔现场走线与未决风险审计 | DESIGN COMPLETE；未覆盖的参考边界、耦合/短桩评审与制板厂阻抗测试条仍未解决 |
| PCB9 | 热计划与验收限值 | ANALYTICAL；U2 热模型必须在 MPN/焊盘图形 ECO 之后重做，然后进行温箱测试 |
| PCB10 | 预合规计划 | COMPLETE；需要实验室扫描 |
| PCB11 | 含 Gerber、钻孔、BOM、位置与图纸的制造目录 | COMPLETE |
| PCB12 | 下单包、DFM 响应字段与自动化发布审计 | NOT ORDER READY；U2 MPN/焊盘图形冻结以及批准、供应商、安全与实物闸门仍未关闭 |
| PCB13 | 36/48/60 V 电源轨、UV/OV/反接/短路/瞬态、急停/CAN 捕获、四小时浸泡与受控固定装置接入计划 | HOLD：需要七个测试接入 ECO 焊盘与已组装原型证据 |
| PCB14 | 预认证/认证报告 | HOLD：需要认可实验室 |
| PCB15 | 已签署的 ECN 与生产 Gerber 文件 | HOLD：需要 PCB12-14 关闭 |
| PCB16 | 20 块已组装板卡与 AOI/X 光记录 | HOLD：需要生产 |
| PCB17 | 温度循环与振动报告 | HOLD：需要测试设施 |
| PCB18 | 受控的组装、测试、返工与证据流程 | COMPLETE |
