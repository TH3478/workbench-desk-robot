# pick_place

把物体 A 放进容器 B。

这是 v0.1 演示任务。验证器检查空间包含：物体是否在托盘包围盒内，且置信度足够？

验证器：`tasks/pick_place/verifier.py`（→ `services/world_model`）
场景：`sim/scenarios/frozen/normal-*.json`、`occlusion-*.json` 等。
