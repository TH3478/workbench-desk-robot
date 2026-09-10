# 感知（Owner: Perception）

P0 用 OpenCV 加 AprilTag 或颜色识别，从受控的 Gazebo 相机发出 `Observation`。必需字段记录在 `interfaces/json_schema/observation.schema.json`。

不要在传感器模式指标中使用仿真器 Oracle 值。

`workbench_perception.ObservationIngestionAdapter` 是 Observation 生产者与世界模型事件流之间失败即拒绝的边界。调用方必须指明已批准的相机标定修订与位姿单位，并注入与 Observation 同域的时钟。适配器在调用其 World Model sink 之前拒绝格式错误、过期、未来偏移、低置信度、重复、未标定与坐标系不匹配的记录。它在 `payload.raw_observation` 下保留未修改的 JSON Observation，并在 `payload.pose` 发出标定位姿；它绝不直接写 `WorldState` 事实。
