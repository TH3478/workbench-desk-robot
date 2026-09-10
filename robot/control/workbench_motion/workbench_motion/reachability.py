"""纯（无 ROS）可达性采样与通过/失败逻辑。

阶段 1 验收闸门（PLAN.md §阶段 1）：在块区采样 >=20 个目标、托盘区 >=20 个目标，
运行 IK，并要求*每个区域* >=95% 成功率——数值与 RNG 种子归档。

本模块拥有其中确定性、可测试的一半：区域是什么、位置如何采样、候选接近 yaw，以及
每个目标的 IK 结果如何对照阈值打分。它没有任何 ROS 导入，因此在未安装 MoveIt 的纯
``uv run pytest`` 下即可运行并单元测试。ROS 半边——实际调用 MoveIt ``/compute_ik``
——位于 ``workbench_motion/reachability_check.py`` 并导入本模块。

这里「可达」的含义（诊断后确定，见 ``candidate_yaws``）：一个采样*位置*可抓取，当且
仅当**至少一个**俯视接近 yaw 产生无碰撞、IK 有效的解。平行爪抓取在 180° 旋转下
对称，且抓取规划器自行选择腕部滚转，因此要求任意指定的 yaw 无碰撞（早期错误）测的
是系统从不使用的能力。脚本仍归档纯 IK 原始可达率与每个位置的无碰撞 yaw 数，使更
严格的视角保持可见。

坐标系：所有位姿都在工作台 ``world`` 坐标系。姿态为俯视抓取（grasp_tcp z 轴向桌面、
世界 -z），并带绕竖直轴的 yaw。区域边界是 Motion 在块起始区与托盘开口上的采样体积；
数值追溯至 robot/description/FRAMES.md（台面顶位于世界 z=0.75，模块起始点靠近台面
(-0.15, 0.05)，托盘靠近台面 (0.22, -0.10)，托盘深 0.05）。
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass

# 绕 x 轴旋转 pi 的纯旋转的四元数 (x, y, z, w)：把 +z 轴映射到 -z，
# 即工具笔直朝下。yaw 在其上复合。
_FLIP_DOWN_XYZW = (1.0, 0.0, 0.0, 0.0)


@dataclass(frozen=True)
class Pose:
    """世界坐标系中的目标位姿：位置（米）+ 四元数 (x, y, z, w)。"""

    x: float
    y: float
    z: float
    qx: float
    qy: float
    qz: float
    qw: float


@dataclass(frozen=True)
class Region:
    """世界坐标系中轴对齐的采样盒（米）。

    位姿在各轴 [min, max] 内均匀采样。z 带是台面上方的悬停范围而非台面本身，
    因为抓取/放置是从上方接近目标的。
    """

    name: str
    x_min: float
    x_max: float
    y_min: float
    y_max: float
    z_min: float
    z_max: float

    def __post_init__(self) -> None:
        if self.x_min > self.x_max or self.y_min > self.y_max or self.z_min > self.z_max:
            raise ValueError(f"region {self.name!r} has an inverted bound")


# 默认采样区域。来源见模块 docstring / FRAMES.md。
# 块区：模块起始点周围 10 cm 的片区，悬停在台面上方。
BLOCK_REGION = Region(
    name="block",
    x_min=-0.20,
    x_max=-0.10,
    y_min=0.00,
    y_max=0.10,
    z_min=0.80,
    z_max=0.90,
)
# 托盘区 = 合法*放置目标*，源自 FRAMES.md，而非为了通过而调参。
# 腔体内部为 0.228 (x) x 0.168 (y)，以台面处托盘基座 (0.22, -0.10) 为中心；
# 沿口顶约在世界 z 0.80。放置目标是腔体内部按夹爪间隙内缩（Robotiq 2F-85 指尖
# 半宽约 0.043 m + 约 0.007 余量 ≈ 每侧 0.05），使指尖避开壁面，并悬停在沿口上方，
# 让 40 mm 的模块下降时能通过开口：
#   x: 0.22 ± (0.114 - 0.05) = [0.156, 0.284]
#   y: -0.10 ± (0.084 - 0.05) = [-0.134, -0.066]
#   z: [0.86, 0.92]  （TCP 位于 0.80 沿口上方）
# 这就是为什么早前 [0.13, 0.31] x [-0.17, -0.03] 带在一个角落失败的原因：其远端
# x 边缘 (0.31) 距远壁约 1 cm 且处于机械臂可达极限——任何腕部 yaw 都无法避开，
# 而且那里本就不是会放置模块的位置。
TRAY_REGION = Region(
    name="tray",
    x_min=0.156,
    x_max=0.284,
    y_min=-0.134,
    y_max=-0.066,
    z_min=0.86,
    z_max=0.92,
)


def default_regions() -> list[Region]:
    """阶段 1 的两个采样区域：块起始区与托盘开口。"""
    return [BLOCK_REGION, TRAY_REGION]


def candidate_yaws(count: int = 12) -> tuple[float, ...]:
    """覆盖 ``[0, pi)`` 的确定性俯视接近 yaw。

    平行爪抓取在 180° 腕部滚转（yaw 与 yaw+pi 是同一物理抓取）下对称，因此不同的
    接近方向位于 ``[0, pi)``。抓取规划器可自由选择其中任意一个；可达性问的是某个
    位置上是否*至少一个*无碰撞且 IK 有效（见模块 docstring / ``reachability_check``
    脚本）。诊断显示纯 IK 可达 100% 位姿，且无碰撞 yaw 形成连续弧段，因此早前逐
    yaw 指标报告的失败是「指定一个系统无需遵守的任意腕部滚转」造成的假象。
    """
    if count <= 0:
        raise ValueError("count must be positive")
    return tuple(i * math.pi / count for i in range(count))


def _quat_mul(
    a: tuple[float, float, float, float],
    b: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    """两个 (x, y, z, w) 四元数的 Hamilton 乘积 -> (x, y, z, w)。"""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def top_down_quat(yaw: float) -> tuple[float, float, float, float]:
    """带世界 z 轴 yaw 的俯视抓取姿态，以 (x, y, z, w) 表示。

    按 Rz(yaw) * Rx(pi) 复合：先把工具翻转向下，再绕世界竖直轴转 yaw。返回结果
    总是归一化的。
    """
    half = yaw / 2.0
    q_yaw = (0.0, 0.0, math.sin(half), math.cos(half))
    qx, qy, qz, qw = _quat_mul(q_yaw, _FLIP_DOWN_XYZW)
    norm = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw)
    return (qx / norm, qy / norm, qz / norm, qw / norm)


def sample_positions(region: Region, n: int, rng: random.Random) -> list[tuple[float, float, float]]:
    """用 ``rng`` 在 ``region`` 内均匀采样 ``n`` 个位置。

    对给定种子的 ``rng`` 是确定性的——同一种子复现相同位置，这正是归档的可达性数值
    可以回放的原因。姿态经 :func:`candidate_yaws` 单独处理，因为一个位置可抓取当且
    仅当*任一*接近 yaw 可行（见模块 docstring）。
    """
    if n <= 0:
        raise ValueError("n must be positive")
    positions: list[tuple[float, float, float]] = []
    for _ in range(n):
        x = rng.uniform(region.x_min, region.x_max)
        y = rng.uniform(region.y_min, region.y_max)
        z = rng.uniform(region.z_min, region.z_max)
        positions.append((x, y, z))
    return positions


def poses_at(position: tuple[float, float, float], yaws: tuple[float, ...]) -> list[Pose]:
    """为 ``position`` 上的每个 yaw 构建俯视候选位姿。"""
    x, y, z = position
    poses: list[Pose] = []
    for yaw in yaws:
        qx, qy, qz, qw = top_down_quat(yaw)
        poses.append(Pose(x=x, y=y, z=z, qx=qx, qy=qy, qz=qz, qw=qw))
    return poses


@dataclass(frozen=True)
class RegionResult:
    """一个区域的 IK 打分结果。"""

    name: str
    total: int
    successes: int
    threshold: float

    @property
    def rate(self) -> float:
        return self.successes / self.total if self.total else 0.0

    @property
    def passed(self) -> bool:
        return self.total > 0 and self.rate >= self.threshold


def score_region(name: str, successes: list[bool], threshold: float = 0.95) -> RegionResult:
    """把区域逐位姿布尔 IK 结果对照 ``threshold`` 打分。"""
    if not successes:
        raise ValueError(f"region {name!r} has no results to score")
    return RegionResult(
        name=name,
        total=len(successes),
        successes=sum(1 for ok in successes if ok),
        threshold=threshold,
    )


def all_passed(results: list[RegionResult]) -> bool:
    """仅当每个区域都达到阈值（且至少有一个）时为真。"""
    return bool(results) and all(r.passed for r in results)


# 阶段 1 验收闸门自身的参数（PLAN.md §阶段 1）：每区域 >=20 个位置、>=95% 通过
# 阈值。参数更弱的运行仍可用于探查，但绝不能作为闸门通过展示。
GATE_MIN_SAMPLES = 20
GATE_MIN_THRESHOLD = 0.95


def validate_run_params(*, samples: int, yaws: int, threshold: float) -> None:
    """直接拒绝无意义的参数（抛 ValueError）。

    这些是独立于闸门的硬边界：取这些值的运行不只是弱，而是无意义（零/负数量、
    [0, 1] 之外的阈值）。闸门强度是单独、更软的检查——见 :func:`is_gate_qualifying`。
    """
    if samples < 1:
        raise ValueError(f"--samples must be >= 1, got {samples}")
    if yaws < 1:
        raise ValueError(f"--yaws must be >= 1, got {yaws}")
    if not 0.0 <= threshold <= 1.0:
        raise ValueError(f"--threshold must be in [0, 1], got {threshold}")


def is_gate_qualifying(*, samples: int, threshold: float) -> bool:
    """一次运行的参数是否强到可算作阶段 1 闸门通过。

    `--samples 1 --threshold 0` 的绿色结果真实但作为验收信号毫无价值；此谓词防止
    这类运行在归档报告中被盖闸门资格章。
    """
    return samples >= GATE_MIN_SAMPLES and threshold >= GATE_MIN_THRESHOLD
