# 第三方评审登记表

| 依赖或资产 | 版本 / 摘要 | 许可证已核查 | 负责人 | 退出路径 |
|---|---|---:|---|---|
| ROS 2 Jazzy | 待定 | 待定 | Linux | 固定版本容器 |
| Gazebo Harmonic | 待定 | 待定 | Linux + 仿真 | 单一仿真器基线 |
| JetPack 6.2.1 / Jetson Linux 36.4.4 | NVIDIA 厂商发行版 | 再分发条款待定 | Linux | 固定厂商包并保留许可证声明 |
| MoveIt 2 | `ros-jazzy-moveit` 2.12.4 | Apache-2.0（2026-08-08 已核查） | 运动 | 固定的已验证轨迹 |
| UR 描述（机械臂 URDF/xacro） | `ros-jazzy-ur-description` | **BSD-3-Clause（代码）— 通过** | 运动 | 仅使用基本图元描述 |
| UR 网格（视觉/碰撞 STL/DAE） | 随 `ur_description/meshes` 发布 | **专有："Universal Robots A/S' Terms and Conditions for Use of Graphical Documentation" — 非开源；分发未经验证** | 运动 + 法务 | 移除视觉网格 / 改用基本图元碰撞几何，或切换到 Panda（仅配置，见 ADR-0004） |
| Robotiq 2F-85 夹爪（描述） | `ros-jazzy-robotiq-description` | BSD（2026-08-08 已核查） | 运动 | 仅使用基本图元夹爪描述 |
| TRAC-IK 运动学插件 | `ros-jazzy-trac-ik-kinematics-plugin` | BSD（上游 trac_ik）— 固定版本时复核 | 运动 | 回退到 KDL（随 MoveIt 附带） |
| UFACTORY xArm ROS 2 包 | `xArm-Developer/xarm_ros2`（`humble`） | BSD-3-Clause（公开仓库） | 运动 + 集成 | 将厂商控制器保留在适配器之后；确认硬件与固件条款 |
| OpenCV / AprilTag | 待定 | 待定 | 感知负责人 | 已知物体基线 |
| Ollama 运行时镜像 | `sha256:b88c73ace3e115f8ec53dc8761ae1c0aabfa675406e3681786b98757ce050f42` | Apache-2.0（发布时验证） | 运行时 + 集成 | 仅 localhost 端点与内部网络 |
| Qwen2.5 0.5B 权重 | `qwen2.5:0.5b`（本地拉取 397 MB） | 模型卡评审待定 | 运行时 + 产品 | 移除模型配置，改用模板运行器 |
| Lucide 图标 | 0.468.0 | ISC（`apps/dashboard/vendor/LUCIDE-LICENSE.txt`） | 交互 | 替换为文本标签 |
| MonoSim | 受邀访问；版本待定 | 使用邀请已记录；许可证与再分发条款待定 | 仿真 + 集成 | 保留在外部适配器之后；条款不允许分发则从发布中移除 |
| RLSOK | 受邀访问；版本待定 | 使用邀请已记录；许可证与再分发条款待定 | 仿真 + 集成 | 保留在外部适配器之后；条款不允许分发则从发布中移除 |
| RLSOK 公开仓库 | `realitywarden/rlsok`（`main`） | Apache-2.0（公开仓库） | 安全 + 集成 | 部署前验证发布版本与托管服务条款 |

模型权重、CAD、网格、图片、音频与代码另行评审。

MonoSim 与 RLSOK 被登记为受邀的第三方集成，而非共同创建的
项目资产。在维护者以书面形式确认适用的许可证、发布与再分发
条款之前，不得将其源码、模型或数据集纳入仓库，也不得声称联合开发。
