# fault_injection

用于向仿真运行注入故障的脚本与配置。

支持四类故障（v0.1）：
- `occlusion`      物体被部分遮挡，相机看不见
- `target_moved`   任务中途物体被移动
- `grasp_failed`   夹爪闭合但物体掉落
- `low_confidence` 相机返回置信度低于阈值的检测

故障注入在场景清单中声明（`fault_type` 字段），由 `services/world_model` 通过注册的钩子触发。

新增故障类：
1. 向 `scenario.schema.json` 的 enum 加入类型字符串
2. 在 `services/world_model/workbench_world_model/faults.py` 实现钩子
3. 添加一个使用新类型的场景清单
4. 添加一个验证故障被触发的测试
