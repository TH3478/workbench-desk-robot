# 机器人控制（Owner: Motion）

在这里实现语义动作适配器与 ActionResult 生产。本模块接收已验证的动作；
它必须拒绝来自 Agent Runtime 的原始关节命令。

详细施工计划与验收闸门：`PLAN.md`（保存在本地，不进入公开仓库）。

## 包：`workbench_motion`

ROS 2（Jazzy）`ament_python` 包。**阶段 1** 交付 UR5e + Robotiq 2F-85
机械臂组合到世界模型、MoveIt 集成，以及可达性闸门
（方块 + 托盘区域上 IK 成功率 ≥95%）：

```
workbench_motion/
  package.xml            # ament_python；ROS 运行时依赖（rclpy、moveit、ur/robotiq 等）
  setup.py / setup.cfg   # colcon 入口点：scaffold_node、reachability_check
  resource/…             # ament index 标记
  workbench_motion/
    logging_setup.py     # 统一 stdlib 日志：run_id/action_id，不使用 print
    evidence.py          # ExecutionEvent + EvidenceSink 接口 + FakeEvidenceSink
    scaffold_node.py     # 空世界自测用最小节点
    reachability.py      # 纯逻辑 IK 采样、区域评分（无 ROS 依赖，已单元测试）
    reachability_check.py  # ROS 控制台脚本：MoveIt /compute_ik，写出评测 JSON
  config/
    arm.yaml             # 换臂配置面：组名、连杆、关节数、基座放置
    arm_on_workbench.urdf.xacro  # UR5e + Robotiq + 工作台世界的组合
    moveit/              # SRDF、运动学（TRAC-IK）、joint_limits、OMPL 流水线
  launch/
    scaffold.launch.py   # 阶段 0 空世界自测
    move_group.launch.py # 组合机械臂的 MoveIt move_group（阶段 1）
  test/                  # pytest 单元测试（证据、日志、可达性逻辑）
```

### 刻意保留两条工具链

- **纯 Python 层**（证据、日志、契约、测试、工具）由 **uv** 管理，
  范围限定在本模块（`robot/control/pyproject.toml` + `uv.lock`）。
  按 `PLAN.md`，根级 `uv.lock` 需要 Linux/集成负责人签字；在此之前
  uv 保持包级局部使用，本环境即如此。
- **ROS 2 运行时**（rclpy、launch、launch_ros，以及后续 moveit2 / ros2_control）
  由 apt/rosdep 管理，并在 `workbench_motion/package.xml` 中声明。uv
  不管理这些依赖。

`evidence.py` 与 `logging_setup.py` 刻意**不引入 ROS 导入**，
`scaffold_node.py` 在 `main()` 内惰性导入 `rclpy`，因此纯 Python
部分可以在没有安装 ROS 的纯净 uv venv 中导入与测试。

## 构建与测试

### 前置条件（阶段 1）

安装 ROS 2 Jazzy、UR/Robotiq 描述以及带 TRAC-IK 的 MoveIt：

```bash
sudo apt-get update && sudo apt-get install -y \
  ros-jazzy-moveit \
  ros-jazzy-moveit-py \
  ros-jazzy-trac-ik-kinematics-plugin \
  ros-jazzy-ur-moveit-config \
  ros-jazzy-ur-simulation-gz \
  ros-jazzy-gz-ros2-control \
  ros-jazzy-controller-manager \
  ros-jazzy-joint-trajectory-controller \
  ros-jazzy-robotiq-controllers
```

（假定 `ros-jazzy-desktop`、`xacro`、`ur-description`、`robotiq-description`
已安装。）

### 纯 Python 单元测试（无需 ROS）

在 `robot/control/` 下：

```bash
uv sync
uv run pytest        # -> workbench_motion/test
```

> 无论是否 source 了 ROS 2 环境都可以运行。当 source 了 ROS 时，其 pytest
> 插件（`launch_testing`、`launch_ros`、`ament_*`）会经 `PYTHONPATH` 混入，
> 并在这个隔离 venv 里导入失败；pytest 配置按名称屏蔽了它们。
> 万一将来配置漂移，可用兜底方案：`PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 uv run pytest`。

### ROS 构建 + 结构校验

需要 source ROS 2 Jazzy；在 `src/` 包含本包的 colcon 工作区中运行：

```bash
# 构建
colcon build --packages-select workbench_motion
source install/setup.bash

# 校验组合后的 URDF（以 world 为根的单棵树，无错误）。组合 xacro 通过
# $(find workbench_motion) 找到 vendored 的 workbench 世界，因此无论是从
# 已安装的 share 目录还是源码树运行都可以（source 工作区后）——无需
# --symlink-install。
xacro $(ros2 pkg prefix workbench_motion)/share/workbench_motion/config/arm_on_workbench.urdf.xacro > /tmp/wb_arm.urdf
check_urdf /tmp/wb_arm.urdf

# 启动 move_group（无头模式，用于可达性检查）
ros2 launch workbench_motion move_group.launch.py

# 在另一个 shell 中（等 move_group 起来后）。--timeout 0.05 与
# config/moveit/kinematics.yaml 中的求解器超时一致。默认 --output 是相对路径，
# 锚定到 git 仓库根目录（而非 shell 的 cwd），因此即使从 robot/control 运行，
# JSON 也会落到仓库里：
source install/setup.bash
ros2 run workbench_motion reachability_check --seed 0 --samples 20 --yaws 12 --timeout 0.05
# 写出 docs/evaluation/phase1-reachability.json；两个区域均 ≥95% 时退出码为 0。
# 传 --output <path> 可覆盖（绝对路径原样使用）。
```

> **状态（2026-08-09 验证，Jazzy 上的 MoveIt + TRAC-IK）：** 闸门通过。
> block 20/20、tray 20/20 可抓取（≥95%），纯 IK 可达两个区域均 100%，
> 无碰撞 yaw 裕量 block 8–11 / tray 11–12。种子 0/7/42 下稳定。
> 该指标是*位置级*的：一个位置只要有 ≥1 个俯视接近 yaw 无碰撞且 IK 有效
> （平行爪抓取对 180° 取模对称，由规划器选择 yaw）就算可抓取。
> `docs/evaluation/phase1-reachability.json` 保存了 seed-0 运行结果。
> 用上面两条命令即可复现。

阶段 0 空世界自测（仍然可用）：

```bash
ros2 launch workbench_motion scaffold.launch.py
```

### 换机械臂（阶段 1 配置面）

机械臂特有标识都隔离在 **`config/arm.yaml`** 与 xacro 参数中，
绝不硬编码进适配器逻辑。日后换成 Panda（或其他机械臂）时只需改动：

1. **`config/arm.yaml`**：规划组、base/ee/gripper 连杆、关节数、基座放置。
2. **`config/arm_on_workbench.urdf.xacro`**：xacro include + 宏实例化（UR → Panda）。
3. **`config/moveit/*`**：SRDF 组、运动学、joint_limits（重新生成或手改）。
4. **`package.xml`**：把 `ur_description` + `robotiq_description` 换成 `franka_description`。
5. **可达性复验**：用新机械臂运行 `reachability_check`，确认 ≥95%。

运行时 Python（`reachability_check.py` 及未来的运动节点）通过
`workbench_motion.arm_config.load_arm_config()` 读取 `config/arm.yaml`——
规划组、IK 末端与基座坐标系**没有**硬编码。CLI 参数可以临时覆盖，
但不传参数时取值来自 arm.yaml（由 `test/test_arm_config.py` 回归守护）。
注意 SRDF 与 xacro 宏实例化仍按机械臂手工编辑——那是「配置编辑」，
不是 Python 编辑。验收标准：换臂不触碰 `workbench_motion/workbench_motion/`
下的任何 `.py` 文件。

## 阶段 2：限位 + ros2_control

安装 Jazzy/Harmonic 运行时包（apt/rosdep，不用 uv）：

```bash
sudo apt-get install -y \
  ros-jazzy-gz-ros2-control \
  ros-jazzy-controller-manager \
  ros-jazzy-joint-trajectory-controller \
  ros-jazzy-joint-state-broadcaster \
  ros-jazzy-gripper-controllers \
  ros-jazzy-ros-gz-sim \
  ros-jazzy-ros-gz-bridge \
  ros-jazzy-ros2controlcli
```

在 ROS 2 Jazzy/Noble 上，这些包会通过 `ros-jazzy-gz-*-vendor` 依赖链
拉入 Gazebo Harmonic。不需要单独的 `gz-harmonic` 包（在纯 ROS 的 apt
配置中它可能并不存在）。`ros_gz_bridge` 只用于 `use_sim_time=true` 节点
所需的 Gazebo→ROS `/clock` 桥接；碰撞证据仍完全来自 MoveIt 的
`/check_state_validity`，不使用 Gazebo 接触桥接。

夹爪包是 `gripper_controllers`，而其 Jazzy 插件类型是历史遗留的
`position_controllers/GripperActionController`。它不是真实硬件的
`robotiq_controllers` 插件。

机械臂 JTC 显式设定了每个关节 `0.02 rad` 的目标容差与 `0.5 s` 的
目标时间裕量。这样不可达、被硬限位截断的目标会以动作中止结束，
而不是被报告为成功的截断目标。

构建并验证可选的（opt-in）控制扩展：

```bash
source /opt/ros/jazzy/setup.bash
colcon build --packages-select workbench_motion
source install/setup.bash
xacro $(ros2 pkg prefix workbench_motion)/share/workbench_motion/config/arm_on_workbench.urdf.xacro \
  sim_gz:=true > /tmp/wb_ctrl.urdf
check_urdf /tmp/wb_ctrl.urdf
ros2 launch workbench_motion sim_control.launch.py
```

在第二个 source 过的 shell 中验证控制器类型/状态并运行单次证据探针：

```bash
ros2 control list_controller_types | grep -i gripper
ros2 control list_controllers
ros2 run workbench_motion phase2_probe
```

`phase2_probe` 只有在 robot_description 包含
`gz_ros2_control/GazeboSimSystem` 时才会发送它的超限测试。观察到控制器
行为后，它会原子地发布 `docs/evaluation/phase2-controllers.json`。
`clamped` 仍算控制器保护闸门失败，并记录为阶段 4 的旁路风险，
但不阻塞阶段 2 验收。真实超限、超时或未分类行为会发布诊断证据并
以退出码 1 结束。端点缺失、数据过期、碰撞或 mimic 失败则以退出码 2
结束，且不发布新产物。

换臂还必须同步更新 `config/controllers.yaml` 中的关节列表与名称，
复核 `config/joint_limits.hw_override.yaml`，并重跑本探针。
厂商硬限位始终是动态的；绝不要把它们的值复制到这里。

## Issue 57：确定性轨迹预检

`workbench_motion.joint_limits.preflight_trajectory` 是唯一无 ROS 依赖的
轨迹闸门。它接收不可变的 `PreflightContext`，用稳定的 `ReasonCode` 拒绝
格式错误或不安全的输入，并返回 `AcceptedTrajectory`，其中包含深度冻结的
规范化快照、规范字节与 SHA-256 证据。`check_trajectory` 仍是同一实现的
阶段 2 兼容 `Violation | None` 包装。

版本化阈值位于 `config/trajectory_preflight.yaml`。期望关节顺序来自
`config/arm.yaml`；生效限位仍是受控厂商限位与硬件覆盖的交集。
策略/限位来源缺失或无效时，以 `invalid_policy` 或 `invalid_limits`
报告就绪失败，而不是当作普通轨迹违规。

无需 ROS、Gazebo 或网络即可运行纯闸门测试：

```bash
uv run --directory robot/control pytest -q \
  workbench_motion/test/test_joint_limits.py \
  workbench_motion/test/test_trajectory_preflight.py \
  workbench_motion/test/test_phase2_probe.py
```

下游 Issue #52 必须只向其执行端口暴露 `AcceptedTrajectory`，并从
`AcceptedTrajectory.snapshot` 物化控制器消息。它还必须在派发前把
已接受的轨迹/上下文证据与当前就绪状态比对。运行时状态/场景的
TOCTOU 复查、控制器物化、零派发证明、#59 拒绝派发映射、C3b 采样
碰撞闸门、执行监控、停止证据与物理安全仍属下阶段工作；
Issue #57 不做任何 ROS、Gazebo 或物理执行声明。
