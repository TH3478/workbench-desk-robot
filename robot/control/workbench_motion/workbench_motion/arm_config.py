"""``config/arm.yaml`` 的加载器——机械臂身份的单一来源。

第一阶段承诺（README + ADR-0004）是：更换机械臂只改配置，绝不改 Python。只有让需要
规划组、IK 末端、基座坐标系、关节列表与模型名的 Python 代码*从 arm.yaml 读取*而不是
硬编码时，这一承诺才成立。本模块就是这条读取路径。

在已 source 的 ROS 工作区（``ament_index``）下运行时，从安装后的包 share 目录解析
``arm.yaml``；否则回退到本文件所在包的源码内副本，使源码检出 / uv venv 环境也能
工作。不需要任何 ROS *运行时*导入（rclpy）；``ament_index_python`` 是轻量纯 Python
查找，而且它本身也是可选的（有保护），因此本模块及其调用方在完全没有 ROS 的环境中
仍然可导入——单元测试直接加载字面路径。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

# 源码内位置：本文件位于 workbench_motion/workbench_motion/arm_config.py；
# 配置目录为 workbench_motion/config/arm.yaml（向上两级，再进 config）。
_IN_SOURCE_ARM_YAML = Path(__file__).resolve().parent.parent / "config" / "arm.yaml"


@dataclass(frozen=True)
class ArmConfig:
    """arm.yaml 的类型化视图。属性访问，而非在字典里翻找。"""

    model: str
    vendor_description_pkg: str
    ur_type: str
    planning_group: str
    base_link: str
    ee_link: str
    ik_tip_link: str
    joints: tuple[str, ...]
    base_frame: str
    gripper_model: str
    gripper_group: str
    driver_joint: str
    update_rate_hz: int
    joint_state_broadcaster: str
    arm_trajectory_controller: str
    gripper_controller: str

    @property
    def dof(self) -> int:
        return len(self.joints)

    @property
    def arm_label(self) -> str:
        """证据归档用的稳定标签，例如 ``ur5e+robotiq_2f_85``。"""
        return f"{self.model}+{self.gripper_model}"


def _find_arm_yaml() -> Path:
    """定位 arm.yaml：先找安装后的 share 目录，再回退到源码内副本。"""
    try:
        from ament_index_python.packages import (
            PackageNotFoundError,
            get_package_share_directory,
        )

        try:
            share = Path(get_package_share_directory("workbench_motion"))
            candidate = share / "config" / "arm.yaml"
            if candidate.is_file():
                return candidate
        except PackageNotFoundError:
            pass
    except ImportError:
        pass
    if _IN_SOURCE_ARM_YAML.is_file():
        return _IN_SOURCE_ARM_YAML
    raise FileNotFoundError("could not locate config/arm.yaml (installed share or in-source)")


def parse_arm_config(data: dict[str, Any]) -> ArmConfig:
    """从解析后的 arm.yaml 映射构建 :class:`ArmConfig`。

    与文件 IO 分离，使单元测试可以在字面字典上验证 映射->对象 的契约，而不触碰
    文件系统或 ROS。
    """
    try:
        arm = data["arm"]
        gripper = data["gripper"]
        controllers = data["controllers"]
    except KeyError as exc:
        raise ValueError(f"missing required arm configuration section: {exc.args[0]}") from exc
    placement = data.get("base_placement", {})
    try:
        model = arm["model"]
        joints = tuple(arm["joints"])
        vendor_description_pkg = arm["vendor_description_pkg"]
        ur_type = arm["ur_type"]
        planning_group = arm["planning_group"]
        base_link = arm["base_link"]
        ee_link = arm["ee_link"]
        ik_tip_link = arm["ik_tip_link"]
        gripper_model = gripper["model"]
        gripper_group = gripper["planning_group"]
        driver_joint = gripper["driver_joint"]
        update_rate_hz = controllers["update_rate_hz"]
        joint_state_broadcaster = controllers["joint_state_broadcaster"]
        arm_trajectory_controller = controllers["arm_trajectory_controller"]
        gripper_controller = controllers["gripper_controller"]
    except (KeyError, TypeError) as exc:
        missing = exc.args[0] if isinstance(exc, KeyError) else "invalid mapping"
        raise ValueError(f"missing or invalid required arm configuration field: {missing}") from exc
    if not joints:
        raise ValueError("arm.joints must be non-empty")
    text_fields = {
        "arm.vendor_description_pkg": vendor_description_pkg,
        "arm.ur_type": ur_type,
        "gripper.driver_joint": driver_joint,
        "controllers.joint_state_broadcaster": joint_state_broadcaster,
        "controllers.arm_trajectory_controller": arm_trajectory_controller,
        "controllers.gripper_controller": gripper_controller,
    }
    for label, value in text_fields.items():
        if not isinstance(value, str) or not value:
            raise ValueError(f"{label} must be a non-empty string")
    if isinstance(update_rate_hz, bool) or not isinstance(update_rate_hz, int) or update_rate_hz <= 0:
        raise ValueError("controllers.update_rate_hz must be a positive integer")
    return ArmConfig(
        model=model,
        vendor_description_pkg=vendor_description_pkg,
        ur_type=ur_type,
        planning_group=planning_group,
        base_link=base_link,
        ee_link=ee_link,
        ik_tip_link=ik_tip_link,
        joints=joints,
        # IK/规划坐标系是底座所固定的工作台世界根坐标系。
        base_frame=placement.get("frame", "world"),
        gripper_model=gripper_model,
        gripper_group=gripper_group,
        driver_joint=driver_joint,
        update_rate_hz=update_rate_hz,
        joint_state_broadcaster=joint_state_broadcaster,
        arm_trajectory_controller=arm_trajectory_controller,
        gripper_controller=gripper_controller,
    )


def load_arm_config(path: Path | str | None = None) -> ArmConfig:
    """加载并解析 arm.yaml。``path`` 覆盖自动发现（供测试使用）。"""
    yaml_path = Path(path) if path is not None else _find_arm_yaml()
    data = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    return parse_arm_config(data)
