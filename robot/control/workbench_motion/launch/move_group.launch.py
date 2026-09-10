"""为合成 UR5e + Robotiq 机械臂启动 move_group（阶段 1）。

目的：提供 ``/compute_ik``（以及合并 URDF 的规划场景），使 ``reachability_check``
console script 能运行批量 IK 可达性闸门，并能在 RViz 中检查机械臂。这是 Motion 自己
的最小 launch——它不依赖任何外部启动调试（PLAN.md §阶段 1）。

路径解析：一切都通过 ``ament_index``（``get_package_share_directory``）从*安装后*的
包 share 解析，因此在普通 ``colcon build`` 之后即可工作（不仅限于
``--symlink-install``）。setup.py 把配置树与工作台世界 xacro 的 vendored 副本安装到
share 目录，合成 xacro 通过 ``$(find workbench_motion)`` 找到世界——运行时完全不依赖
源码树布局。需要先 source 工作区::

    colcon build --packages-select workbench_motion
    source install/setup.bash
    ros2 launch workbench_motion move_group.launch.py

机械臂 xacro 路径以可覆盖、已校验的 launch 参数暴露。

依赖：ros-jazzy-moveit、ros-jazzy-trac-ik-kinematics-plugin（kinematics.yaml）。
"""

from __future__ import annotations

from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from workbench_motion.launch_utils import move_group_parameters, require_file, robot_description

_SHARE = Path(get_package_share_directory("workbench_motion"))
_CONFIG_DIR = _SHARE / "config"
_DEFAULT_ARM_XACRO = str(_CONFIG_DIR / "arm_on_workbench.urdf.xacro")


def _setup(context, *_args, **_kwargs) -> list[Node]:
    arm_xacro = Path(LaunchConfiguration("arm_xacro").perform(context))
    require_file(arm_xacro, "arm xacro")
    description = robot_description(arm_xacro)
    moveit_params = move_group_parameters(_SHARE, description, use_sim_time=False)

    move_group = Node(
        package="moveit_ros_move_group",
        executable="move_group",
        output="screen",
        parameters=moveit_params,
    )
    rsp = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        output="screen",
        parameters=[description, {"use_sim_time": False}],
    )
    # 静态关节状态使 TF 对 IK/碰撞完整（尚无控制器；ros2_control 在阶段 2 落地）。
    jsp = Node(
        package="joint_state_publisher",
        executable="joint_state_publisher",
        output="screen",
        parameters=[{"use_sim_time": False}],
    )
    return [rsp, jsp, move_group]


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        [
            DeclareLaunchArgument("arm_xacro", default_value=_DEFAULT_ARM_XACRO),
            OpaqueFunction(function=_setup),
        ]
    )
