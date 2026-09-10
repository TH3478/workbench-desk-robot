# 硬件现场验证包

本包是仿真/工程证据到真实机器的交接。它不声称存在机器人、实验室或
48 小时运行。每个测量模板初始为 `NOT_EXECUTED`，只有在附上签署的证据记录
后才能变为 `PASS` 或 `FAIL`。

- `sim2real-matrix.csv` 定义对比维度和验收带宽。
- `diagnostic-sop.md` 是面向操作员的 CAN、电源、传感器、温度和安全停止
  故障决策树。
- `fault-scenarios.csv` 包含 20 个带恢复和证据要求的故障注入场景。
- `first-batch-acceptance.csv` 是十台设备的验收模板。
- `long-run-protocol.md` 定义 48 小时可靠性运行和停止规则。

运行 `python hardware/validation/tools/validate_validation.py` 重新生成
确定性报告。

## 证据注册

先为 `first-batch-acceptance.csv` 中的设备分配真实的 `hardware_revision`
和 64 字符的配置/固件哈希。然后注册原始证据：

```bash
python hardware/validation/tools/register_evidence.py \
  --evidence-id EVT-VAL5-01-001 --scenario-id VAL5-01 --unit-id UNIT-001 \
  --operator OPERATOR --reviewer REVIEWER --captured-at 2026-08-18T08:00:00Z \
  --evidence-kind physical --instrument-ref CAN-SCOPE-01 \
  --calibration-ref CAL-2026-001 --raw-file runs/hardware/val5-01.log --result PASS
```

该命令把 SHA-256 哈希存入 `evidence-register.jsonl`。当场景或设备未知、
修订版本与受控设备行不一致、文件缺失或被修改、或证据 ID 被重用时，验证
采取失败即拒绝。编辑汇总 CSV 不能把场景提升为 `PASS`。
