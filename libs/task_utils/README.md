# task_utils

跨 Workbench 任务边界共享的标准库工具。

当前已实现：

- `exclusive_file_lock`：持久的 sidecar 文件锁，POSIX 上由 `flock` 支撑，Windows 上由单字节 `msvcrt` 锁支撑。它同时串行化线程与进程，且使用后不删除锁文件。

```python
from workbench_task_utils import exclusive_file_lock

with exclusive_file_lock("state.json.lock"):
    update_state()
```

空间包含、位姿比较、置信度聚合与 evidence-ref 助手仍处于规划中，只会在重复出现的验证器模式需要它们时添加。
