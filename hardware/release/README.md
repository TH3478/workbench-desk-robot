# 硬件发布就绪闸门

这是工程包的统一发布视图。它将现有的 PCB 与制造报告同采购、QA 与现场验证报告
合并在一起。绿色的工程检查并不意味着实物产品发布：商业、安全、实验室、生产与
现场闸门在附上带日期的证据之前保持阻塞。

报告暴露两个独立的阶段。`EVT_PROTOTYPE_ORDER_*` 涵盖原型下单之前所需的工程、
Owner、供应商与设计闸门。`PRODUCTION_RELEASE_*` 额外要求实物启动调试、实测安全
时序、线束执行与已验证的固定装置接入。这种分离防止实物证据成为其必须由原型
产生的循环前提；在外部证据缺失时，仅靠仓库校验器不会把任一阶段标记为就绪。

运行：

```bash
python hardware/release/tools/check_release_readiness.py
```

默认命令是生产发布闸门，在生产被阻塞时返回非零值。EVT 原型下单闸门使用
`--stage evt`；只有在审计治理 schema 而不请求发布决策时，才使用
`--stage structure`。JSON 支撑的行声明一个 `evidence_binding`；检查器会拒绝与所
引用报告中绑定的工程、EVT、生产或实物结果字段不一致的 CSV `PASS`。

该命令写出 `generated/release_readiness_report.json`。只用受控记录更新
`evidence-register.csv`。每条外部记录都必须标明设备/批次、版本、操作员、日期、
仪器或供应商来源，以及原始证据路径。

`hardware-closure-checklist.csv` 是面向轴、电源、安全、PCB、线束、机械、验证与
合规 Owner 的机器可读主清单。其 `evt_order_blocker` 与
`production_release_blocker` 列使阶段边界清晰，同时让所有实物与商业未知项保持
失败即拒绝。

`hardware-optimization-register.csv` 是针对剩余 P0/P1/P2 风险的跨域行动登记册。
每行将当前状态绑定到一项具体优化、一个 Owner、一份验收证据契约与其影响的发布
阶段。校验命令：

```bash
python hardware/release/tools/validate_optimization_register.py
```

该登记册刻意不是发布豁免：只有在其列出的证据附入常规发布证据登记册且上游报告
重新生成之后，项目才能标记为 `CLOSED`。
