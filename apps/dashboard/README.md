# 看板（Owner: Interaction）

Workbench-1 的只读任务状态、证据与回放 UI。

```bash
python -m workbench_backend.server --host 127.0.0.1 --port 8080
```

打开 `http://127.0.0.1:8080`。运行可通过 `?run=<run_id>` 深链；包裹固定装置使用 `?run=dashboard-parcel--parcel-intake-003`。后端默认提供已入库的固定装置运行，并可用 `--data-dir` 指向任何包含有序 `.jsonl` 事件流的目录。

包裹运行包含一个只读决策表，展示观测到的标签与状态、由 policy 推导的目的地，以及实际放置是否匹配该决策。

HTTP 边界刻意只实现 `GET`。`POST`、`PUT`、`PATCH` 与 `DELETE` 返回 `405 read_only`；本应用中没有 ROS、MCU、运动或急停发布者。

内置 UI 依赖：Lucide `0.468.0`，ISC 许可证在 `vendor/LUCIDE-LICENSE.txt`。

看板遵循双标签页键盘模型：`Left`/`Right`（或 `Up`/`Down`）切换视图，`Home` 与 `End` 跳到第一个或最后一个视图。过滤器与运行选择暴露按下状态，回放暴露播放与位置状态，活跃的移动运行滚动到视野内。当操作系统请求减少动效时，非必要的动画被抑制。
