from glob import glob

from setuptools import find_packages, setup

package_name = "workbench_motion"

setup(
    name=package_name,
    version="0.2.0",
    packages=find_packages(exclude=["test", "test.*"]),
    data_files=[
        ("share/ament_index/resource_index/packages", ["resource/" + package_name]),
        ("share/" + package_name, ["package.xml"]),
        ("share/" + package_name + "/launch", glob("launch/*.launch.py")),
        ("share/" + package_name + "/config", glob("config/*.yaml") + glob("config/*.xacro")),
        ("share/" + package_name + "/config/moveit", glob("config/moveit/*")),
        # 工作台世界 xacro 归 robot/description 所有（不是 ROS 包，没有
        # package.xml）。我们在构建时*把一份副本内置到我们的 share 目录*，使组装后
        # 的 URDF 在源码空间与安装空间都能通过 $(find workbench_motion) 解析。为什么
        # $(find robot/description) 不可行，见 config/arm_on_workbench.urdf.xacro。
        ("share/" + package_name + "/description", ["../../description/workbench.urdf.xacro"]),
    ],
    install_requires=["setuptools"],
    zip_safe=True,
    maintainer="Motion Owner",
    maintainer_email="motion-owner@workbench-1.invalid",
    description="运动语义动作适配器包：UR5e + Robotiq 机械臂组成与可达性（阶段 1）。",
    license="Apache-2.0",
    tests_require=["pytest"],
    entry_points={
        "console_scripts": [
            "scaffold_node = workbench_motion.scaffold_node:main",
            "reachability_check = workbench_motion.reachability_check:main",
            "phase2_probe = workbench_motion.phase2_probe:main",
        ],
    },
)
