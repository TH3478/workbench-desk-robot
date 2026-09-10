# 脚本化失败与恢复用例

这些是确定性接口固定装置，不是 Gazebo 或硬件结果。其目的是在集成存在之前，让失败语义、证据要求与看板行为保持可评审。

## 相机被遮挡：证据不足

运行：`run-uncertain`<br>
证据：`apps/dashboard/data/run-uncertain.jsonl`

运动适配器报告 `succeeded`，但最后一次观测置信度为 `0.41`。验证器返回 `insufficient_evidence`，将 `fresh_camera_frame` 与 `target_confidence_above_0.80` 列为缺失，并建议 `re_observe`。看板渲染为 `uncertain`，绝不会是 `failed` 或 `pleased`。

## 首次抓取失败：结果判定为未满足

运行：`run-recovery`，序列 `3-4`<br>
证据：`apps/dashboard/data/run-recovery.jsonl`

首次抓取失去接触。动作结果为 `failed`；验证器单独返回 `refuted`，并同时引用相机帧与运动日志。派发状态不被视为物理完成。

## 恢复成功：历史保持可见

运行：`run-recovery`，序列 `5-9`<br>
证据：`apps/dashboard/data/run-recovery.jsonl`

第二次尝试产生新的动作结果与新鲜观测。只有此时验证器才发出 `confirmed`。回放保留先前被判定未满足的结论，因此操作员可以检查两次尝试，而不是看到被改写成只有成功的历史。

正式 D7 评审仍需要 36 次带真实日志的 Gazebo 运行与独立的虚假完成审计。`tools/scripts/run_evaluation.py --runner external` 是集成边界；`--runner scripted` 始终写入 `release_eligible: false`。
