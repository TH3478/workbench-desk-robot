# 采购工程包

本工程包将 PCB、机械与制造设计输入转化为受控的采购工作流。它刻意如实反映已知
与未知：标记为 `QUOTE_REQUIRED`、`AVL_REQUIRED` 或 `NOT_RELEASED` 的行都不是采购
承诺。采购员可以使用报价登记册与 PO 检查清单关闭这些闸门，而不改变工程基线。

## 文件

- `bom.csv`：受控的逐行 BOM，含数量、候选、来源、Owner 与发布状态。
- `quote-register.csv`：每个关键物料的两个或更多询价渠道；在收到报价之前供应商
  价格保持空白。
- `supplier-scorecard.csv`：可重复的质量/交期/成本/技术评审。
- `cost-and-leadtime.md`：计算规则与决策闸门。
- `po-checklist.csv`：下单发布检查与所需证据。
- `inventory-policy.md`：收货、隔离、可追溯性与备件规则。

运行 `python hardware/procurement/tools/validate_procurement.py` 可在 `generated/`
下重新生成确定性报告。
