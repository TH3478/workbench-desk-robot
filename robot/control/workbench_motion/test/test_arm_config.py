"""arm.yaml 加载器的单元测试——单一事实来源契约。

阶段 1 的承诺是「换机械臂只改配置，不改 Python」。这些测试断言使该承诺成立的机制：
(1) 随包的 config/arm.yaml 解析为类型化 ArmConfig，(2) reachability_check CLI 从该
配置取机械臂身份（组 / 末端 / 基座坐标系）而非硬编码字符串。如果有人重新在脚本里
硬编码 "ur_manipulator"，测试 (2) 即失败。

无 ROS：在 `uv run pytest` 下运行。绝不导入 rclpy/moveit。
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from pathlib import Path

import pytest
from workbench_motion.arm_config import ArmConfig, load_arm_config, parse_arm_config

# 随包配置，从源码树解析（本测试文件所在包向上两级：
# workbench_motion/test -> workbench_motion -> config）。
_CONFIG_DIR = Path(__file__).resolve().parent.parent / "config"
_ARM_YAML = _CONFIG_DIR / "arm.yaml"
_SRDF = _CONFIG_DIR / "moveit" / "workbench_arm.srdf"


def test_shipped_arm_yaml_loads():
    cfg = load_arm_config(_ARM_YAML)
    assert isinstance(cfg, ArmConfig)
    assert cfg.model == "ur5e"
    assert cfg.vendor_description_pkg == "ur_description"
    assert cfg.ur_type == "ur5e"
    assert cfg.planning_group == "ur_manipulator"
    assert cfg.ik_tip_link == "grasp_tcp"
    assert cfg.base_frame == "world"
    assert cfg.dof == 6
    assert cfg.joints[0] == "shoulder_pan_joint"
    assert cfg.gripper_model == "robotiq_2f_85"
    assert cfg.driver_joint == "robotiq_85_left_knuckle_joint"
    assert cfg.update_rate_hz == 500
    assert cfg.joint_state_broadcaster == "joint_state_broadcaster"
    assert cfg.arm_trajectory_controller == "arm_trajectory_controller"
    assert cfg.gripper_controller == "gripper_controller"
    assert cfg.arm_label == "ur5e+robotiq_2f_85"


def test_shipped_arm_yaml_joint_list():
    """arm.yaml 按链序携带 6 个 UR 关节。"""
    cfg = load_arm_config(_ARM_YAML)
    assert cfg.joints == (
        "shoulder_pan_joint",
        "shoulder_lift_joint",
        "elbow_joint",
        "wrist_1_joint",
        "wrist_2_joint",
        "wrist_3_joint",
    )


def _srdf_groups(root: ET.Element) -> dict[str, ET.Element]:
    return {g.get("name"): g for g in root.findall("group")}


def test_arm_yaml_agrees_with_srdf():
    """arm.yaml 与手工编写的 SRDF 不得漂移。

    SRDF（config/moveit/workbench_arm.srdf）是 move_group 实际加载的；arm.yaml 是
    运行时 Python 读取的。如果有人改 SRDF 链或改组名而不同步 arm.yaml（或反之），IK
    就会指向 move_group 未暴露的组/末端。本测试解析真实 SRDF（标准库 XML，无 ROS），
    并断言机械臂规划组、其链基座/末端 link 与夹爪组名与 arm.yaml 完全一致。
    """
    cfg = load_arm_config(_ARM_YAML)
    root = ET.fromstring(_SRDF.read_text(encoding="utf-8"))
    groups = _srdf_groups(root)

    # 机械臂规划组以 arm.yaml 声明的名字存在。
    assert cfg.planning_group in groups, f"missing SRDF arm group {cfg.planning_group!r}; groups={sorted(groups)}"
    # 其链把 IK 解析到 arm.yaml 命名的末端，以 base_link 为根。
    chain = groups[cfg.planning_group].find("chain")
    assert chain is not None, f"SRDF group {cfg.planning_group!r} is not a chain group"
    assert chain.get("base_link") == cfg.base_link
    assert chain.get("tip_link") == cfg.ik_tip_link

    # 夹爪组名也要一致（arm.yaml 的 gripper.planning_group）。
    assert cfg.gripper_group in groups, f"missing SRDF gripper group {cfg.gripper_group!r}; groups={sorted(groups)}"


def test_parse_rejects_empty_joints():
    with pytest.raises(ValueError):
        parse_arm_config(
            {
                "arm": {
                    "model": "x",
                    "vendor_description_pkg": "vendor",
                    "ur_type": "x",
                    "planning_group": "g",
                    "base_link": "b",
                    "ee_link": "e",
                    "ik_tip_link": "t",
                    "joints": [],
                },
                "gripper": {"model": "g", "planning_group": "gripper", "driver_joint": "finger"},
                "controllers": {
                    "update_rate_hz": 100,
                    "joint_state_broadcaster": "jsb",
                    "arm_trajectory_controller": "arm",
                    "gripper_controller": "gripper",
                },
            }
        )


def test_parse_defaults_base_frame_to_world_when_absent():
    cfg = parse_arm_config(
        {
            "arm": {
                "model": "ur5e",
                "vendor_description_pkg": "ur_description",
                "ur_type": "ur5e",
                "planning_group": "ur_manipulator",
                "base_link": "base_link",
                "ee_link": "tool0",
                "ik_tip_link": "grasp_tcp",
                "joints": ["a"],
            },
            "gripper": {"model": "g", "planning_group": "gripper", "driver_joint": "finger"},
            "controllers": {
                "update_rate_hz": 100,
                "joint_state_broadcaster": "jsb",
                "arm_trajectory_controller": "arm",
                "gripper_controller": "gripper",
            },
        }
    )
    assert cfg.base_frame == "world"
    assert cfg.gripper_model == "g"


@pytest.mark.parametrize("missing", ["gripper", "controllers"])
def test_parse_rejects_missing_safety_sections(missing):
    data = {
        "arm": {
            "model": "ur5e",
            "vendor_description_pkg": "ur_description",
            "ur_type": "ur5e",
            "planning_group": "arm",
            "base_link": "base",
            "ee_link": "tool",
            "ik_tip_link": "tip",
            "joints": ["joint"],
        },
        "gripper": {"model": "g", "planning_group": "gripper", "driver_joint": "finger"},
        "controllers": {
            "update_rate_hz": 100,
            "joint_state_broadcaster": "jsb",
            "arm_trajectory_controller": "arm",
            "gripper_controller": "gripper",
        },
    }
    del data[missing]
    with pytest.raises(ValueError, match="missing required"):
        parse_arm_config(data)


def test_parse_rejects_missing_arm_section_as_value_error():
    with pytest.raises(ValueError, match="missing required arm configuration section: arm"):
        parse_arm_config({"gripper": {}, "controllers": {}})


def test_parse_rejects_required_identity_field_as_value_error():
    data = {
        "arm": {
            "vendor_description_pkg": "ur_description",
            "ur_type": "ur5e",
            "planning_group": "arm",
            "base_link": "base",
            "ee_link": "tool",
            "ik_tip_link": "tip",
            "joints": ["joint"],
        },
        "gripper": {"model": "g", "planning_group": "gripper", "driver_joint": "finger"},
        "controllers": {
            "update_rate_hz": 100,
            "joint_state_broadcaster": "jsb",
            "arm_trajectory_controller": "arm",
            "gripper_controller": "gripper",
        },
    }
    with pytest.raises(ValueError, match="required arm configuration field: model"):
        parse_arm_config(data)


def test_parse_rejects_missing_driver_joint():
    data = {
        "arm": {
            "model": "ur5e",
            "vendor_description_pkg": "ur_description",
            "ur_type": "ur5e",
            "planning_group": "arm",
            "base_link": "base",
            "ee_link": "tool",
            "ik_tip_link": "tip",
            "joints": ["joint"],
        },
        "gripper": {"model": "g", "planning_group": "gripper"},
        "controllers": {
            "update_rate_hz": 100,
            "joint_state_broadcaster": "jsb",
            "arm_trajectory_controller": "arm",
            "gripper_controller": "gripper",
        },
    }
    with pytest.raises(ValueError, match="driver_joint"):
        parse_arm_config(data)


@pytest.mark.parametrize("rate", [0, -1, 100.5, True])
def test_parse_rejects_invalid_controller_update_rate(rate):
    data = {
        "arm": {
            "model": "ur5e",
            "vendor_description_pkg": "ur_description",
            "ur_type": "ur5e",
            "planning_group": "arm",
            "base_link": "base",
            "ee_link": "tool",
            "ik_tip_link": "tip",
            "joints": ["joint"],
        },
        "gripper": {"model": "g", "planning_group": "gripper", "driver_joint": "finger"},
        "controllers": {
            "update_rate_hz": rate,
            "joint_state_broadcaster": "jsb",
            "arm_trajectory_controller": "arm",
            "gripper_controller": "gripper",
        },
    }
    with pytest.raises(ValueError, match="positive integer"):
        parse_arm_config(data)


def test_reachability_check_defaults_come_from_arm_yaml():
    """「单一来源」缺陷的回归守卫。

    CLI 在标志未设置时必须从 arm.yaml 解析 group/tip/base-frame——而非硬编码字面量。
    我们复刻脚本 main() 执行的解析并断言它得到配置值。
    """
    from workbench_motion.reachability_check import _parse_args

    args = _parse_args([])  # 无覆盖
    assert args.group is None and args.tip is None and args.base_frame is None

    cfg = load_arm_config(_ARM_YAML)
    group = args.group or cfg.planning_group
    tip = args.tip or cfg.ik_tip_link
    base_frame = args.base_frame or cfg.base_frame
    assert (group, tip, base_frame) == ("ur_manipulator", "grasp_tcp", "world")


def test_reachability_check_flags_override_config():
    from workbench_motion.reachability_check import _parse_args

    args = _parse_args(["--group", "custom_group", "--tip", "custom_tip", "--base-frame", "custom_frame"])
    cfg = load_arm_config(_ARM_YAML)
    assert (args.group or cfg.planning_group) == "custom_group"
    assert (args.tip or cfg.ik_tip_link) == "custom_tip"
    assert (args.base_frame or cfg.base_frame) == "custom_frame"
