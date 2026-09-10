# mobile_base

移动底盘集成。验证器契约泛化到导航目标，因此加入移动底盘无需改动验证层。

规划于 v0.4。验证器将检查「机器人在容差内到达目标位姿，且定位置信度已确认」。

子目录（实现后）：
- `description/`   底盘 URDF / TF
- `control/`       ros2_control 硬件接口或 Nav2 集成
- `bringup/`       launch 文件
