# 冻结的 P1 场景

本目录包含 12 个不可变的 v0.1 评测清单：

- normal: 3；
- occlusion / low confidence: 3；
- moving target: 3；
- grasp failure: 3。

每个清单必须包含稳定的场景 ID、种子、世界版本、超时与故障类型。期望结果与生成器、候选模型提示分开存放。

同一批 12 个清单对每个被比较的系统版本运行。`python tools/scripts/validate_scenarios.py` 还会把每个种子物化两次，并拒绝分布漂移或非确定性。

`expanded/` 目录为路径阻塞、低光照与多个同色物体新增 18 个 P2 清单。它不修改本冻结基线。
