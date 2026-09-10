# HW1 官方 UR5e 提取证据

状态：**EXECUTED**，于 2026-08-12 在仓库开发环境中执行。

本证据使用 ROS Jazzy 的 `ur_description` 包作为受控的
非生成来源。展开后的 URDF 是临时生成的工件，
不被视为限值的来源。

## 受控来源引用

- `ros-jazzy-ur-description 3.5.1-1noble.20260615.175716`
- `ros-jazzy-xacro 2.1.1-1noble.20260519.011123`
- `/opt/ros/jazzy/share/ur_description/urdf/ur.urdf.xacro`
  SHA-256: `d2eb6b60edf4c18b347c0612598f3c2b95fd0b189cf16fdb8c2f2ac119a0a82f`
- `/opt/ros/jazzy/share/ur_description/config/ur5e/joint_limits.yaml`
  SHA-256: `1e908454a0fb073761a0c675f708eb003ca7d5333a9e0863ef87fe40d9abb3c7`

该包的 `joint_limits.yaml` 以 Universal Robots e-Series UR5e 用户
手册与 Universal Robots 的最大关节扭矩文章作为其上游
来源。

## 复现

```bash
/opt/ros/jazzy/bin/xacro \
  /opt/ros/jazzy/share/ur_description/urdf/ur.urdf.xacro \
  ur_type:=ur5e name:=ur5e \
  -o /tmp/workbench-hw1-official-ur5e.urdf

python3 libs/hardware/urdf_to_motor_config.py \
  /tmp/workbench-hw1-official-ur5e.urdf \
  --joint shoulder_pan_joint \
  --joint shoulder_lift_joint \
  --joint elbow_joint \
  --joint wrist_1_joint \
  --joint wrist_2_joint \
  --joint wrist_3_joint
```

展开后的 URDF 的 SHA-256 为
`a21bf5fb70b3a1745bf2e4816e0f43654539737ba7f2585939ec773d159778d8`，
大小为 11,870 字节，恰好包含六个旋转关节。每个选中的
关节都恰好包含一个 `<limit>` 元素。它不包含传动信息，
因此以下所有减速比都明确未知。

下方提取出的 YAML 的 SHA-256 为
`9a32af8b6504b4e242d2f3bf9458d5a87c96ae45fb55a14e459644fe6c5fd7d1`，
大小为 1,394 字节。

## 提取出的电机配置

```yaml
motors:
  - name: "shoulder_pan_joint"
    joint_type: "revolute"
    max_torque_nm: 150.0
    max_velocity_rad_s: 3.141592653589793
    lower_limit_rad: -6.283185307179586
    upper_limit_rad: 6.283185307179586
    mechanical_reduction: null
  - name: "shoulder_lift_joint"
    joint_type: "revolute"
    max_torque_nm: 150.0
    max_velocity_rad_s: 3.141592653589793
    lower_limit_rad: -6.283185307179586
    upper_limit_rad: 6.283185307179586
    mechanical_reduction: null
  - name: "elbow_joint"
    joint_type: "revolute"
    max_torque_nm: 150.0
    max_velocity_rad_s: 3.141592653589793
    lower_limit_rad: -3.141592653589793
    upper_limit_rad: 3.141592653589793
    mechanical_reduction: null
  - name: "wrist_1_joint"
    joint_type: "revolute"
    max_torque_nm: 28.0
    max_velocity_rad_s: 3.141592653589793
    lower_limit_rad: -6.283185307179586
    upper_limit_rad: 6.283185307179586
    mechanical_reduction: null
  - name: "wrist_2_joint"
    joint_type: "revolute"
    max_torque_nm: 28.0
    max_velocity_rad_s: 3.141592653589793
    lower_limit_rad: -6.283185307179586
    upper_limit_rad: 6.283185307179586
    mechanical_reduction: null
  - name: "wrist_3_joint"
    joint_type: "revolute"
    max_torque_nm: 28.0
    max_velocity_rad_s: 3.141592653589793
    lower_limit_rad: -6.283185307179586
    upper_limit_rad: 6.283185307179586
    mechanical_reduction: null
```
