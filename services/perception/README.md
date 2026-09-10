# 感知（Owner: Perception）

P0 用 OpenCV 加 AprilTag 或颜色识别，从受控的 Gazebo 相机发出 `Observation`。必需字段记录在 `interfaces/json_schema/observation.schema.json`。

不要在传感器模式指标中使用仿真器 Oracle 值。

`workbench_perception.ObservationIngestionAdapter` 是 Observation 生产者与世界模型事件流之间失败即拒绝的边界。调用方必须指明已批准的相机标定修订与位姿单位，并注入与 Observation 同域的时钟。适配器在调用其 World Model sink 之前拒绝格式错误、过期、未来偏移、低置信度、重复、未标定与坐标系不匹配的记录。它在 `payload.raw_observation` 下保留未修改的 JSON Observation，并在 `payload.pose` 发出标定位姿；它绝不直接写 `WorldState` 事实。

## 有界观测属性

`Observation.attributes` 是一个可选的、带版本的字符串到字符串映射。它刻意设计为有限契约，而不是任意的 JSON 属性袋：

| 范围 | 支持的键 |
| --- | --- |
| 通用实体 | `colour`、`presence`、`identity`、`orientation` |
| 包裹实体 | `label_status`、`condition`、`tracking_id`、`barcode`、`parcel_uid` |
| 家电实体 | `door_state`、`rack_state`（洗衣机或洗衣机门支持 `door_state`；洗碗机两者都支持） |
| 受管槽位 | `slot_state`、`slot_occupancy` |

未知实体类型只获得通用键。生产者在发出事件之前必须拒绝未知键、不适用于该实体类型的键、无效枚举值、空文本或带首尾空白的文本、控制字符、无效 UTF-8 以及超尺寸的值。限制为 32 个属性、64 字符的键、256 字符的值，以及规范化 UTF-8 JSON 的 4096 字节。有界枚举为：

| 键 | 允许的值 |
| --- | --- |
| `label_status` | `verified`、`unreadable`、`missing`、`unknown` |
| `condition` | `intact`、`damaged`、`unknown` |
| `door_state`、`rack_state` | `open`、`closed`、`unknown` |
| `slot_state`、`slot_occupancy` | `empty`、`occupied`、`blocked`、`unknown` |
| `presence` | `present`、`absent`、`unknown` |

每个现代属性值都携带匹配的 `attribute_metadata`。元数据包含 `observed_at`、`[0, 1]` 内的有限置信度、1 到 32 个唯一证据引用、一个信念（`observed`、`inferred`、`stale` 或 `lost`）、一个 `clock_id`（`monotonic` 或 `wall`）以及可选来源。元数据文本与证据引用最多 256 字符；规范化元数据 JSON 最多 16 KiB。现代版本标记为 `observed-attributes-v1`。显式的 `legacy-observed-attributes-v0` 标记仅为迁移旧包裹载荷而存在；它不允许任意键或值。

在摄取边界上，带有属性但缺少逐属性元数据的现代记录，可以从外层观测的时间戳、置信度、证据、时钟与来源物化元数据。规范 JSON Schema 与直接 Pydantic 模型仍然要求现代载荷携带元数据，因此通过其他生产者进入的记录不能静默绕过证据契约。旧载荷可以保持稀疏，直到 reducer 的显式迁移路径处理它们。

`attributes_mode` 是 `complete` 或 `partial`。完整观测是替换快照；部分观测是逐键补丁，并且只有在该实体已有完整基线之后才被接受。两种模式都不允许动作结果创建或刷新观测属性。
