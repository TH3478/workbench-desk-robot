"""阶段 0 自测 launch：在空世界中启动 scaffold 节点。

刻意不依赖任何外部组件（无启动调试、无 Gazebo、无机械臂）。它只证明包能安装、
其节点能启动并记录日志。后续阶段加入机械臂 + 世界 launch。
"""

from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        [
            Node(
                package="workbench_motion",
                executable="scaffold_node",
                name="workbench_motion_scaffold",
                output="screen",
            ),
        ]
    )
