# ADR-0004: 机械臂选型——UR5e + Robotiq 2F-85

## 状态

v0.1 阶段 1（Motion）已接受。取代 PLAN.md 偏向 Panda 的倾向；原因见“决策”。

## 背景

Motion 计划的阶段 1（`robot/control/PLAN.md` §阶段 1）要求选择一个**官方厂商**机械臂资产——
明确*不是*自拼 URDF——并将其组合进 `robot/description/workbench.urdf.xacro` 的工作台世界。
该任务必须在 Ubuntu 24.04 + ROS 2 Jazzy + Gazebo Harmonic、无 GPU 下运行。

PLAN.md 偏向 Franka Panda（7 DoF，资产最丰富），以 UR5e（6 DoF）为备选，但把型号选择留给
我们，待厂商包验证（`THIRD_PARTY_REVIEW.md` 行）。我们按计划的指示（“先验证 vendor 包再
定型”），在决定前验证了可用性与许可证。

## 已验证事实（2026-08-08 核查，ROS 2 Jazzy）

| 事实 | Panda | UR5e |
|---|---|---|
| 官方描述包 | `ros-jazzy-moveit-resources-panda-description`（测试资源，不是维护中的厂商线） | `ros-jazzy-ur-description`（维护中的 Universal Robots 线） |
| 本环境已安装 | 否 | **是**（`ur_description`、`robotiq_description`） |
| apt 上的 MoveIt 配置包 | `moveit-resources-panda-moveit-config`（仅测试） | `ros-jazzy-ur-moveit-config`（维护中） |
| 夹爪 | 集成 `franka_hand` | **无**——需要附加组件 |
| Gazebo 仿真辅助 | 无第一方 | `ros-jazzy-ur-simulation-gz` |
| URDF/xacro 许可证 | Apache-2.0（干净） | BSD-3-Clause（干净） |
| 网格许可证 | BSD-ish（moveit 资源） | **专有——“Universal Robots A/S' Terms and Conditions for Use of Graphical Documentation”**（见 THIRD_PARTY_REVIEW） |

## 决策

阶段 1 机械臂使用 **UR5e**，在 UR `tool0` 坐标系上挂载 **Robotiq 2F-85** 平行夹爪
（`ros-jazzy-robotiq-description`）。

理由，按优先级排序：

1. **维护中的官方厂商线，不是测试固定装置。**`ur_description` / `ur_moveit_config` 是维护中
   的 Universal Robots ROS 2 包。Jazzy 上的 Panda 资产是 `moveit-resources-*`——MoveIt 自己的
   *测试*固定装置，不是厂商维护的产品线。计划的意图（“官方机械臂资产，不自己拼 URDF”）由
   维护中的产品线更好地满足。
2. **已安装 + apt 干净的依赖图。**`ur_description` 与 `robotiq_description` 已存在；其余
   （`ur-moveit-config`、`ur-simulation-gz`、`moveit`、`trac-ik`）在 apt 上干净解析（用
   `apt-get install --just-print` 验证，退出码 0）。无源码构建，无 vendoring。
3. **第一方 Gazebo Harmonic 支持**来自 `ur-simulation-gz`，这是 Panda 测试资源所缺的——这为
   阶段 2（ros2_control）和阶段 5（Gazebo 执行）降低风险。
4. **6 DoF 足够**在固定桌子上对 40 mm 方块做自上而下的抓放并放入托盘。Panda 的第 7 DoF
   带来的冗余对本任务并不需要；计划本身也注明 UR5e 是有效选择。

已接受的权衡：UR5e 出厂**不带夹爪**，因此我们加 Robotiq 2F-85。而且 UR 的**网格处于专有
许可证下**——已在 THIRD_PARTY_REVIEW 中标注并给出退出路径（原始碰撞几何 / 模型替换），并未
当作干净。

## 已考虑的替代方案

- **Franka Panda。**集成夹爪与 7 DoF 有吸引力，但 Jazzy apt 上只有测试固定装置资产，无第一方
  Gazebo 辅助，且本环境未安装。保留为文档化后备，以防 UR 网格许可证阻碍分发（替换仅改配置——
  见 README 替换清单）。
- **UR3e / UR10e。**同家族；UR3e 臂展（0.5 m）对同时覆盖方块起始区与托盘而言勉强，UR10e
  （1.3 m）对 1.2 m 桌子过大。UR5e（0.85 m 臂展）以余量适配工作区。
- **自建 URDF。**被计划明确否决——调校自制机械臂的物理参数要多花一周，也违背“官方资产”。

## 后果

- 机械臂特定标识符（规划组、`base_link`、`tool0`/EE link、夹爪组与连杆、关节数、底座放置
  位姿）隔离在 `robot/control/workbench_motion/config/arm.yaml` 与 xacro 参数中。之后换
  Panda 只改配置，绝不动适配器逻辑（阶段 1 验收闸门 + README 替换清单）。
- UR 网格许可证是 THIRD_PARTY_REVIEW 中记录的发布风险。若无法清除，退出路径是仅原始碰撞
  几何（丢弃视觉网格）或文档化的 Panda 替换。
- 可达性按 UR5e 0.85 m 臂展包络验证。底座按 ≥95% IK 闸门（PLAN.md §阶段1）调校：初始后角
  放置 `(-0.42, -0.28)` 运动学上到达 100% 位姿，但把托盘留在臂展极限处，那里每个腕部偏航在
  3/20 个托盘位置都碰撞（85%，失败）。把底座移到 `(-0.30, -0.15, 0.75)`、偏航 `0.36`、面向
  方块+托盘质心，缩短了托盘臂展（约 0.66 m → 约 0.52 m）并消除碰撞：**方块 20/20、托盘
  20/20、均 100%**，在种子 0/7/42 下稳定（`docs/evaluation/phase1-reachability.json`，
  2026-08-09 用 MoveIt + TRAC-IK 验证）。闸门度量是位置级：若 ≥1 个自上而下偏航无碰撞且 IK
  有效，则该位置可抓取——平行爪抓取对 180° 取模对称且由规划器选择偏航，因此要求固定随机偏航
  （早期的一个错误）测量了系统从不使用的能力。
- 世界挂接恰好有 ONE 个来源：合并 URDF 生成的 `base_joint`（world → base_link）。SRDF 刻意
  不为其声明 `virtual_joint`——第二次声明是冗余的，MoveIt 会警告或拒绝一个其子连杆已有 URDF
  父关节的虚拟关节。（该 SRDF 选择由 `check_urdf` 结构性验证；完整确认需要 MoveIt 安装后的
  `move_group` 解析。）

## 带入阶段 3 的阶段 1 跟进项

- **`camera_body` 没有碰撞几何。**在 `robot/description/workbench.urdf.xacro` 中，
  `camera_body` 是手写连杆，只有 `<visual>`——没有 `<collision>`（不同于 `camera_post`，后者
  使用含 collision 的 `static_box` 宏）。因此 MoveIt **不**把相机主体视为障碍物，经过它附近
  的规划可能擦碰它。阶段 1 不受影响：可达性采样区域（方块、托盘）远离相机支柱，且 ≥95% 闸门
  通过。但在阶段 3 全碰撞规划之前，`camera_body` 需要 `<collision>`。该文件归
  `robot/description`（Simulation/description Owner）所有，超出 Motion 的写入范围，因此阶段
  3 要么请该 Owner 添加它，要么在我们这边为相机主体添加单个 PlanningScene 碰撞对象——这是对
  “合并 URDF 是唯一碰撞来源”规则的刻意且有文档的例外，理由正是来源几何缺失。

## 复现

```bash
# vendor packages (already-installed ones omitted)
sudo apt-get install -y ros-jazzy-moveit ros-jazzy-moveit-py \
  ros-jazzy-trac-ik-kinematics-plugin ros-jazzy-ur-moveit-config \
  ros-jazzy-ur-simulation-gz ros-jazzy-gz-ros2-control \
  ros-jazzy-controller-manager ros-jazzy-joint-trajectory-controller \
  ros-jazzy-robotiq-controllers

# structural validation (after colcon build + source install/setup.bash, so
# $(find workbench_motion) resolves the vendored world)
xacro $(ros2 pkg prefix workbench_motion)/share/workbench_motion/config/arm_on_workbench.urdf.xacro > /tmp/wb_arm.urdf
check_urdf /tmp/wb_arm.urdf
```
