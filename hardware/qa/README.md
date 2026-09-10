# 质量工程包

本工程包定义如何检验真实整机、PCB 组件、线束与外壳，以及如何关闭故障。它是受控
的测试与证据计划，并不声称实物设备已经通过。

- `test-standard.md` 定义功能、电气、安全、工艺与可靠性闸门。
- `inspection-plan.csv` 将每道工序映射到方法、样本量、限值与证据记录。
- `fmea.csv` 包含初始失效模式登记册与 Owner。
- `defect-workflow.md` 与 `defect-tracker.csv` 定义 MRB、遏制与纠正措施状态。
- `aql-plan.csv` 定义批次抽样与零容忍安全规则。
- `compliance-matrix.csv` 跟踪 RoHS、EMC/FCC 与电池证据，而不断言尚未颁发的认证。

运行 `python hardware/qa/tools/validate_qa.py` 以重新生成报告。
