# 系统架构

```text
Simulation camera -> Perception Observation -> World Model -> Agent Runtime
                                                  ^                 |
                                                  |                 v
Dashboard <- Read-only HTTP API <- Event Store <- ActionResult <- Motion / Virtual MCU
```

## 运行时单元

1. `robot/`：ROS 2、Gazebo 适配器、MoveIt 与语义动作执行。
2. `services/perception/`：产出 `Observation`；从不写入 WorldState。
3. `services/world_model/`：reducer、验证器、事件库与回放读模型。
4. `services/agent_runtime/`：把用户目标转换为类型化 `TaskGraph`。
5. `firmware/virtual_mcu/`：协议与安全状态模拟器。
6. `services/backend/`：健康/就绪检查，加上有序事件流的只读投影。
7. `apps/dashboard/`：任务状态、四种表情状态、证据检查与确定性回放。

运行时有两个规划器提供方。模板提供方完全离线且确定性；可选的 Ollama 提供方仅限
localhost/容器网络：它返回一个五路路由决策，然后由受信任的确定性构建器产出 `TaskGraph`。
模型响应永远不会成为关节、速度、固件、急停或完成命令。

## 关键流程

1. 感知发出带帧、置信度和来源的 `Observation`。
2. World Model 记录一个 `WorldEvent`，应用确定性 reducer 并对外暴露 `WorldState`。
3. Agent Runtime 从 `TaskGraph` 发出类型化语义动作。
4. Motion 返回带证据引用的 `ActionResult`。
5. World Model 验证预期结果。只有它发出 `VerificationResult`。
6. Backend/回放展示事实与证据；它不推导第二个 WorldState，也不接受控制写入。

## 硬件运行时边界（规划中）

物理路径刻意是一个独立的、由 Owner 把关的层：

```text
device / Linux driver -> DeviceAdapter -> bounded DeviceRuntime
  -> ROS 2 typed boundary -> selected RMW (Fast DDS deployment)
  -> validation + provenance -> World Model / evidence -> read-only API
```

CAN、相机/触控、机械臂与安全适配器共享生命周期、取消、队列和溯源规则，但仍是独立插件。
Fast DDS 是部署传输选择，不是适配器契约的依赖。急停、看门狗、安全使能与直接运动授权仍在
DDS 之外。该边界在 [ADR-0005](../decisions/ADR-0005-hardware-device-runtime.md) 中提出；
本仓库目前尚无 Fast DDS 或物理硬件实现。

## 运行边界

- `/healthz` 报告进程健康；`/readyz` 检查事件源是否可读。
- `/api/v1/runs` 和 `/api/v1/runs/{run_id}/events` 暴露有序读模型；
  `/api/*` 仍作为兼容别名。已签入的 OpenAPI 契约是
  `docs/api-openapi-v1.json`。
- 事件 JSONL 文件按路径、修改时间和大小缓存；文件变化会自动失效。
- 静态响应使用 ETag，而版本化的 vendored 资产使用不可变缓存。
- `POST`、`PUT`、`PATCH` 和 `DELETE` 返回 `405 read_only`。
- 服务日志是 JSON Lines 格式，带 `service`、`source`、`run_id` 和每次运行的 `sequence_no` 字段。相同的记录结构同时接受 `simulation` 和 `hardware` 来源，无需修改分析代码。
- 阶段遥测使用 `event=stage_completed`、`details.stage` 和 `details.duration_ms`；`analyze_telemetry.py` 为两种来源计算 P50/P95。
- 控制器可通过 `WORKBENCH_EVENT_SOURCE_URL` 经 HTTP 读取仿真事件源；当对端不可用或返回畸形事件时，其就绪状态为假。
- `apps/dashboard/data/` 是用于离线 UI 和 API 测试的固定装置数据，永远不能作为物理发布证据。

## 安全边界

Agent Runtime 不能发出关节位置、速度命令、急停决策或物理完成声明。这些分别属于 Motion、Virtual MCU 和 World Model 验证器。

## 硬件在环边界

```text
Linux host -- SocketCAN -- J5/J6 isolated CAN -- controller/fixture
     |                         |
     |                    J10 dual E-stop --> U8 --> J11 safe enable
     `-- evidence logger -----+---- scope/CAN/thermal raw files
                                      |
                         signed evidence register --> release gate
```

HIL 主机可以请求语义动作并记录证据。它不能绕过 U8、伪造安全使能通过，或把缺失的采集变成完成。
物理流程与精确连接器映射见硬件布线与启动调试页面。
