#!/usr/bin/env python3
"""阶段 1 可达性闸门：通过 MoveIt 对块区与托盘区批量 IK。

运行 ``workbench_motion.reachability`` 的确定性采样，并对每个采样位姿调用 MoveIt 的
``/compute_ik`` 服务（针对机械臂规划组，使用 grasp_tcp 末端）。按 >=95% 阈值给每个
区域打分，把数值 + 种子归档到 ``docs/evaluation/phase1-reachability.json``，任一区域
失败即以非零码退出——从而可作为构建闸门。

这是闸门的 ROS 半边；采样/打分半边在
``workbench_motion/test/test_reachability.py`` 中以无 ROS 方式单元测试。rclpy 与
MoveIt 消息在 ``main`` 内惰性导入，使本模块（以及纯逻辑模块）在无 ROS 环境下仍可
导入。

前置条件：合成机械臂的 ``move_group`` 必须已在运行，``/compute_ik`` 才可用。先用
以下命令启动::

    ros2 launch workbench_motion move_group.launch.py

然后在另一个 shell 中（已安装的 console script）::

    ros2 run workbench_motion reachability_check --seed 0 --samples 20

规划组名、末端 link 与规划坐标系默认取 config/arm.yaml 的值。
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from pathlib import Path

# 纯逻辑与 arm.yaml 加载器位于安装后的包中。
from workbench_motion.arm_config import load_arm_config
from workbench_motion.reachability import (
    Pose,
    Region,
    candidate_yaws,
    default_regions,
    is_gate_qualifying,
    poses_at,
    sample_positions,
    score_region,
    validate_run_params,
)


def _resolve_output_path(output: str, start: Path | None = None) -> Path:
    """把 ``--output`` 相对仓库根解析，而非调用方的 CWD。

    文档化运行从 ``robot/control``（``colcon build`` 之后）开始，但归档属于
    ``<repo>/docs/evaluation/...``。绝对路径原样使用；相对路径锚定到 git 仓库根
    （最近的包含 ``.git`` 的祖先目录），使 JSON 无论 console script 从哪里调用都
    落在仓库内。找不到仓库根时回退为原相对路径。``start`` 默认为 CWD，仅为可测试性
    而存在。
    """
    p = Path(output)
    if p.is_absolute():
        return p
    start = start or Path.cwd()
    for parent in [start, *start.parents]:
        if (parent / ".git").exists():
            return parent / p
    return p


def _parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MoveIt batch reachability check")
    p.add_argument("--seed", type=int, default=0, help="RNG seed (archived for replay)")
    p.add_argument("--samples", type=int, default=20, help="positions per region (>=20 for the gate)")
    p.add_argument("--yaws", type=int, default=12, help="candidate approach yaws per position (symmetric [0,pi))")
    # 机械臂身份默认值来自 config/arm.yaml，而非此处硬编码。未设置的标志（None）在
    # main() 中从加载的 ArmConfig 填充；显式传入则覆盖配置（便于临时探查）。
    p.add_argument("--arm-config", default=None, help="path to arm.yaml (default: package share / in-source)")
    p.add_argument("--group", default=None, help="planning group (default: arm.yaml planning_group)")
    p.add_argument("--tip", default=None, help="IK tip link (default: arm.yaml ik_tip_link)")
    p.add_argument("--base-frame", default=None, help="planning/pose frame (default: arm.yaml base_placement.frame)")
    p.add_argument("--threshold", type=float, default=0.95, help="per-region pass rate (gate needs >=0.95)")
    p.add_argument(
        "--probe",
        action="store_true",
        help="allow sub-gate params (samples<20 or threshold<0.95) for ad-hoc probing; "
        "the report is stamped gate_qualifying=false and the process exits non-zero",
    )
    # 默认值与 config/moveit/kinematics.yaml 的 kinematics_solver_timeout
    # （0.05 s）一致：TRAC-IK 求解器在 0.05 即停，更大的单次请求超时也不会给求解器
    # 更多预算。保留为一个旋钮，得到连贯、可复现的数字并记入报告的 ik_timeout_s。
    p.add_argument("--timeout", type=float, default=0.05, help="per-IK timeout seconds (matches solver timeout)")
    p.add_argument(
        "--output",
        default="docs/evaluation/phase1-reachability.json",
        help="archive path; a relative path is anchored to the git repo root "
        "(not the caller's cwd), an absolute path is used verbatim",
    )
    return p.parse_args(argv)


def _build_ik_request(
    moveit_msgs,
    geometry_msgs,
    std_msgs,
    sensor_msgs,
    *,
    pose: Pose,
    group: str,
    tip: str,
    frame: str,
    joints: tuple[str, ...],
    timeout: float,
    collide: bool = True,
):
    req = moveit_msgs.srv.GetPositionIK.Request()
    ik = req.ik_request
    ik.group_name = group
    ik.ik_link_name = tip
    # collide=True -> 碰撞感知（真正的闸门）。collide=False -> 纯运动学可达，
    # 作为诊断一并归档。
    ik.avoid_collisions = collide
    ik.timeout.sec = int(timeout)
    ik.timeout.nanosec = int((timeout - int(timeout)) * 1e9)

    # 按 GetPositionIK 契约填充完整的 robot_state。没有填充 JointState 时，
    # move_group 对每个请求都记录 "Found empty JointState message" 并回退到当前状态；
    # 以中性 0.0 种子提供机械臂关节既满足契约，又使种子显式且可复现。关节名来自
    # arm.yaml（单一来源），而非硬编码。
    js = sensor_msgs.msg.JointState()
    js.name = list(joints)
    js.position = [0.0] * len(joints)
    ik.robot_state.joint_state = js

    ps = geometry_msgs.msg.PoseStamped()
    ps.header = std_msgs.msg.Header()
    ps.header.frame_id = frame
    ps.pose.position.x = pose.x
    ps.pose.position.y = pose.y
    ps.pose.position.z = pose.z
    ps.pose.orientation.x = pose.qx
    ps.pose.orientation.y = pose.qy
    ps.pose.orientation.z = pose.qz
    ps.pose.orientation.w = pose.qw
    ik.pose_stamped = ps
    return req


def _call_ik(node, client, req, moveit_msgs, *, wait: float) -> bool:
    import rclpy

    future = client.call_async(req)
    rclpy.spin_until_future_complete(node, future, timeout_sec=wait)
    if not future.done() or future.result() is None:
        return False
    # moveit_msgs/MoveItErrorCodes 中 SUCCESS == 1。
    return future.result().error_code.val == moveit_msgs.msg.MoveItErrorCodes.SUCCESS


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)

    # arm.yaml 是机械臂身份的单一来源。CLI 标志覆盖它；未设置的标志回退到配置。
    # 下面不硬编码任何机械臂专属字符串。
    arm = load_arm_config(args.arm_config)
    group = args.group or arm.planning_group
    tip = args.tip or arm.ik_tip_link
    base_frame = args.base_frame or arm.base_frame

    # 硬边界：在做任何工作前拒绝无意义的参数。
    validate_run_params(samples=args.samples, yaws=args.yaws, threshold=args.threshold)
    # 软闸门：低于闸门的参数只在 --probe 下允许，且永远不算闸门通过。这阻止
    # `--samples 1 --threshold 0` 产出绿色的验收报告。
    gate_qualifying = is_gate_qualifying(samples=args.samples, threshold=args.threshold)
    if not gate_qualifying and not args.probe:
        print(
            f"refusing sub-gate run (samples={args.samples}, threshold={args.threshold}): "
            f"the phase-1 gate needs samples>=20 and threshold>=0.95. "
            f"Re-run with --probe to explicitly do a non-qualifying probe.",
            file=sys.stderr,
        )
        return 2

    import geometry_msgs.msg
    import moveit_msgs.msg
    import moveit_msgs.srv
    import rclpy
    import sensor_msgs.msg
    import std_msgs.msg
    from rclpy.node import Node

    rclpy.init()
    node = Node("reachability_check")
    client = node.create_client(moveit_msgs.srv.GetPositionIK, "/compute_ik")

    node.get_logger().info("waiting for /compute_ik (is move_group running?)")
    if not client.wait_for_service(timeout_sec=15.0):
        node.get_logger().error("/compute_ik unavailable; launch move_group first")
        node.destroy_node()
        rclpy.shutdown()
        return 2

    rng = random.Random(args.seed)
    regions: list[Region] = default_regions()
    yaws = candidate_yaws(args.yaws)
    started = time.strftime("%Y-%m-%dT%H:%M:%S%z")

    def _ik(pose: Pose, *, collide: bool) -> bool:
        req = _build_ik_request(
            moveit_msgs,
            geometry_msgs,
            std_msgs,
            sensor_msgs,
            pose=pose,
            group=group,
            tip=tip,
            frame=base_frame,
            joints=arm.joints,
            timeout=args.timeout,
            collide=collide,
        )
        return _call_ik(node, client, req, moveit_msgs, wait=args.timeout + 1.0)

    region_reports = []
    results = []
    for region in regions:
        positions = sample_positions(region, args.samples, rng)
        # 每个位姿的可抓取性（闸门）：>=1 个无碰撞且 IK 有效的 yaw。
        graspable: list[bool] = []
        # 保持可见的诊断：纯运动学可达 + 无碰撞 yaw 余量。
        pure_reach: list[bool] = []
        yaw_margins: list[int] = []
        for pos in positions:
            poses = poses_at(pos, yaws)
            free = sum(1 for p in poses if _ik(p, collide=True))
            yaw_margins.append(free)
            graspable.append(free > 0)
            # 纯可达：是否存在忽略碰撞即可求解的 yaw（仅运动学）
            pure_reach.append(any(_ik(p, collide=False) for p in poses))
        res = score_region(region.name, graspable, threshold=args.threshold)
        results.append(res)
        pure_rate = sum(pure_reach) / len(pure_reach) if pure_reach else 0.0
        node.get_logger().info(
            f"region={res.name} graspable={res.successes}/{res.total} "
            f"rate={res.rate:.3f} pass={res.passed} pure_reach={pure_rate:.3f} "
            f"yaw_margin(min/median)={min(yaw_margins)}/{sorted(yaw_margins)[len(yaw_margins) // 2]}"
        )
        region_reports.append(
            {
                "name": res.name,
                "positions": res.total,
                "graspable": res.successes,
                "rate": round(res.rate, 4),
                "threshold": res.threshold,
                "passed": res.passed,
                "pure_reach_rate": round(pure_rate, 4),
                "yaw_margin_min": min(yaw_margins),
                "yaw_margin_max": max(yaw_margins),
                "bounds": {
                    "x": [region.x_min, region.x_max],
                    "y": [region.y_min, region.y_max],
                    "z": [region.z_min, region.z_max],
                },
            }
        )

    passed = bool(results) and all(r.passed for r in results)
    report = {
        "generated_at": started,
        "seed": args.seed,
        "samples_per_region": args.samples,
        "yaws_per_position": args.yaws,
        "ik_timeout_s": args.timeout,
        "metric": "position graspable if >=1 top-down yaw is collision-free and IK-valid",
        "group": group,
        "tip_link": tip,
        "base_frame": base_frame,
        "threshold": args.threshold,
        "arm": arm.arm_label,
        # gate_qualifying=false 标记一次参数过弱、不能作为阶段 1 验收信号的探查运行，
        # 即使所有区域都「通过」。
        "gate_qualifying": gate_qualifying,
        "regions": region_reports,
        "all_passed": passed,
    }

    out = _resolve_output_path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    node.get_logger().info(
        f"archived reachability report to {out} (all_passed={passed}, gate_qualifying={gate_qualifying})"
    )

    node.destroy_node()
    rclpy.shutdown()
    # 只有当区域全部通过且参数具备闸门资格时，运行才算「成功」（rc 0）。
    # 不具备资格的探查以非零码退出，使 CI 永远不能把它当作绿色。
    return 0 if (passed and gate_qualifying) else 1


if __name__ == "__main__":
    sys.exit(main())
