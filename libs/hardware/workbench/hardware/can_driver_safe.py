"""由统一设备运行时托管的受限主机侧 CAN 适配器。

本模块实现 :class:`SafeCANBus`：ADR-0005 统一 ``DeviceRuntime`` 下的 CAN
``DeviceAdapter``。它持有注入的阻塞传输端口（见 :class:`CanTransportPort`，
参考实现为 ``workbench.hardware.socketcan_transport.SocketCANTransport``）
与 CAN Wire V1 协议关联状态，但不拥有生命周期、worker、队列或订阅者注册表——
这些全部归 :class:`DeviceRuntime` 所有。契约来源：
``docs/architecture/host-can-transport-v1.md``（传输契约、失败即拒绝边界、
生命周期）与 ``docs/architecture/mcu-wire-v1.md``（Wire V1 帧布局、仲裁标识符
分区、数字注册表）；帧编解码的权威实现在 ``firmware/mcu/core/frame_codec.c``，
本模块只复制帧跨主机边界所需的有界入口校验，绝不改变线上数字或逻辑协议。

安全与所有权边界（失败即拒绝）：

- 只接受标准 11 位、非远程、非错误、DLC=8 且携带已知 Wire V1 仲裁标识符
  与完整跨字段语义的帧；任何其他帧在发布输出前被整体拒绝。
- 普通命令在途请求至多一条；成功的本地传输分发仅开始确认期限，并不等于
  MCU 已接受命令。重试保持命令关联并递增线上重试计数，预算耗尽后清空普通
  流量并发出关联 STOP。
- STOP 抢占排队与挂起的普通流量；STOP 确认超时、STOP 被拒绝、bus-off 与链路
  丢失都会使适配器在显式恢复成功之前无法发送普通命令。
- 恢复清空故障前挂起帧、绝不回放过期流量；遥测按协议半区间序号排序，重复与
  过期快照被忽略，故障遥测在显式传输恢复之前禁用普通流量。
- 每个有效入站结果携带不可变 :class:`CanTransportEnvelope`（含来源、接口、
  入口序号、单调与墙上时钟观测、链路健康、经过校验的线上帧与证据引用）；
  非有限或倒退的时钟观测是可观测的失败即拒绝入口错误，不会被静默归一化。
"""

from __future__ import annotations

import logging
import math
import threading
import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, replace
from enum import IntEnum, StrEnum
from typing import Protocol
from urllib.parse import quote

logger = logging.getLogger("CANBusSafe")

# Wire V1 帧的载荷字节 0：紧凑协议版本。逻辑版本 "1.0" 编码为 0x10
# （十六进制 10 = 十进制 16，即版本 1.0）。来源：docs/architecture/mcu-wire-v1.md。
MCU_WIRE_VERSION_V1 = 0x10
# 每种 Wire V1 帧类型的 DLC 都恰为 8；远程、extended-ID 与 CAN FD 帧在本编解码器之外。
MCU_WIRE_DLC = 8
# 仲裁标识符分区（mcu-wire-v1.md「仲裁标识符」表）。较低标识符赢得 CAN 仲裁：
# STOP/STOP_ACK 拥有最高协议优先级，命令/ACK 居中，遥测最低。ID 恰好选择一种帧类型，
# 其余标准标识符一律被编解码器拒绝。
MCU_CAN_ID_STOP = 0x080
MCU_CAN_ID_STOP_ACK = 0x081
MCU_CAN_ID_COMMAND = 0x100
MCU_CAN_ID_ACK = 0x101
MCU_CAN_ID_TELEMETRY = 0x180
# 内核 CAN 错误掩码中的 bus-off 位：原始 ID 与 0x1FFFFFFF（CAN_ERR_MASK）按位与后，
# 该位为 1 表示控制器已进入 bus-off 状态。对应 socketcan_transport.CAN_ERR_BUSOFF。
_CAN_ERR_BUSOFF = 0x00000040
# Linux SocketCAN raw-ID 标志位（struct can_frame.can_id 高位）：
# 0x80000000 = CAN_EFF_FLAG（extended 29 位 ID）；
# 0x40000000 = CAN_RTR_FLAG（远程传输请求）；
# 0x20000000 = CAN_ERR_FLAG（错误帧）。
# 三者互斥组合由帧校验路径检查，解码后从 11 位仲裁标识符中剥离。
_CAN_EFF_FLAG = 0x80000000
_CAN_RTR_FLAG = 0x40000000
_CAN_ERR_FLAG = 0x20000000


class CanFrameKind(StrEnum):
    """Wire V1 帧类型，由仲裁标识符一一映射（见 ``_ID_TO_KIND``）。

    五种类型与 mcu-wire-v1.md 的仲裁标识符表对应：``COMMAND``（0x100，
    主机到 MCU）、``ACK``（0x101，MCU 到主机）、``TELEMETRY``（0x180，
    MCU 到主机）、``STOP``（0x080，主机到 MCU）、``STOP_ACK``（0x081，
    MCU 到主机）。入站仅允许 ACK/STOP_ACK/遥测，出站仅允许 COMMAND/STOP。
    """

    COMMAND = "command"
    ACK = "ack"
    TELEMETRY = "telemetry"
    STOP = "stop"
    STOP_ACK = "stop_ack"


class CanLinkState(StrEnum):
    """CAN 链路与协议健康状态；刻意不是第二个生命周期状态机。

    生命周期（configure -> activate -> worker -> deactivate -> cleanup）归
    :class:`DeviceRuntime` 所有，本枚举只报告链路健康，转移由 ``SafeCANBus``
    在运行时锁内完成。转移不变量：

    - ``NEW``：构造后、configure 前。
    - ``STARTING``：activate 已开始；open 成功且运行时仍处于 ACTIVATING 时
      进入 ``ACTIVE``，否则进入 ``SHUTDOWN``。
    - ``ACTIVE``：端口已打开、可发送普通命令与 STOP；打开失败进入 ``LINK_LOST``。
    - ``STOPPING``：有 STOP 在途（显式或 ack 超时升级）；STOP_ACK 成功进入
      ``SAFE_STOPPED``，被拒绝或超时进入 ``LINK_LOST``。
    - ``SAFE_STOPPED``：STOP 已被 MCU 确认，普通命令在显式恢复前被禁用。
    - ``BUS_OFF``：内核报告 bus-off（``_CAN_ERR_BUSOFF``）；挂起工作被清空，
      需显式 ``recover()`` 确认外部完成 CAN 核心重启后才可回到 ``ACTIVE``。
    - ``LINK_LOST``：传输错误、STOP 拒绝/超时、故障遥测或时钟故障等；
      失败即拒绝，普通命令被拒绝直至显式恢复成功。
    - ``SHUTDOWN``：运行时 DEACTIVATING/CLEANED，或关闭已请求。
    """

    NEW = "new"
    STARTING = "starting"
    ACTIVE = "active"
    STOPPING = "stopping"
    SAFE_STOPPED = "safe_stopped"
    BUS_OFF = "bus_off"
    LINK_LOST = "link_lost"
    SHUTDOWN = "shutdown"


class DeviceRuntimeState(StrEnum):
    """由 :class:`DeviceRuntime` 持有的生命周期状态。

    CAN 链路健康有意用 ``CanLinkState`` 单独表示。
    适配器不得把链路状态当作第二个生命周期所有者。
    """

    NEW = "new"
    CONFIGURING = "configuring"
    CONFIGURED = "configured"
    ACTIVATING = "activating"
    ACTIVE = "active"
    DEACTIVATING = "deactivating"
    CLEANED = "cleaned"
    FAILED = "failed"


class CanSendStatus(StrEnum):
    """出站命令准入结果分类。

    - ``QUEUED``：已进入运行时有界命令平面（不表示 MCU 已接受）。
    - ``BACKPRESSURE``：命令平面已满，调用方收到类型化背压。
    - ``INVALID_FRAME``：帧未通过 Wire V1 校验或方向/类型不被允许。
    - ``CORRELATION_CONFLICT``：STOP 已挂在途，或命令 ID 仍在有界关联窗口内。
    - ``NOT_RUNNING``：运行时未激活或已关闭。
    - ``LINK_UNAVAILABLE``：链路健康不允许发送（失败即拒绝）。
    """

    QUEUED = "queued"
    BACKPRESSURE = "backpressure"
    INVALID_FRAME = "invalid_frame"
    CORRELATION_CONFLICT = "correlation_conflict"
    NOT_RUNNING = "not_running"
    LINK_UNAVAILABLE = "link_unavailable"


class CanReceiveStatus(StrEnum):
    """入站事件分类。

    - ``ACCEPTED``：帧通过校验且被关联（ACK/STOP_ACK）或通过遥测序号排序。
    - ``INVALID_FRAME``：帧或传输记录不合法，被拒绝且不进入外部投影。
    - ``DUPLICATE``：重复 ACK/STOP_ACK/遥测快照。
    - ``LATE``：过期 ACK 或过期/歧义遥测；绝不变更较新动作或声称完成。
    - ``UNCORRELATED``：无法关联到任何已派发尝试的确认。
    """

    ACCEPTED = "accepted"
    INVALID_FRAME = "invalid_frame"
    DUPLICATE = "duplicate"
    LATE = "late"
    UNCORRELATED = "uncorrelated"


class CanDiagnosticCode(StrEnum):
    """有界健康平面上的诊断代码。

    记录最旧淘汰、按错误计数累计；数值枚举语义对齐
    docs/architecture/mcu-wire-v1.md 的 Fault 注册表：
    ``ack_timeout``/``stop_timeout`` 仅主机诊断、绝不来自 MCU；
    ``stop_rejected`` 仅失败 STOP_ACK；``link_lost``/``watchdog_expired``
    仅故障遥测；``duplicate_frame``/``malformed_frame`` 仅失败普通 ACK。
    """

    INVALID_FRAME = "invalid_frame"
    BACKPRESSURE = "backpressure"
    CORRELATION_CONFLICT = "correlation_conflict"
    ACK_TIMEOUT = "ack_timeout"
    STOP_TIMEOUT = "stop_timeout"
    STOP_REJECTED = "stop_rejected"
    BUS_OFF = "bus_off"
    LINK_LOST = "link_lost"
    DUPLICATE_ACK = "duplicate_ack"
    LATE_ACK = "late_ack"
    UNCORRELATED_ACK = "uncorrelated_ack"
    COMMAND_REJECTED = "command_rejected"
    DUPLICATE_TELEMETRY = "duplicate_telemetry"
    STALE_TELEMETRY = "stale_telemetry"
    CLOCK_ROLLBACK = "clock_rollback"
    SUBSCRIBER_ERROR = "subscriber_error"
    STOP_PREEMPTED = "stop_preempted"
    SHUTDOWN_TIMEOUT = "shutdown_timeout"
    TRANSPORT_BACKPRESSURE = "transport_backpressure"
    EXTERNAL_BACKPRESSURE = "external_backpressure"


class _WireOpcode(IntEnum):
    """Wire V1 载荷字节 3 的数字注册表（mcu-wire-v1.md「数字注册表」）。

    ``STOP``（0x05）只允许出现在 STOP 帧中；其余为普通命令 opcode。
    ``RESERVED`` 与任何未列出的值都是保留且无效的。
    """

    RESERVED = 0
    MOVE = 1
    GRIP_OPEN = 2
    GRIP_CLOSE = 3
    HOLD = 4
    STOP = 5
    HEARTBEAT = 6


class _WireResult(IntEnum):
    """ACK/STOP_ACK 载荷字节 5 的结果码：accepted（0x00）或 rejected（0x01）。"""

    ACCEPTED = 0
    REJECTED = 1


class _WireFault(IntEnum):
    """Wire V1 Fault 注册表（mcu-wire-v1.md）。

    除 ``NONE`` 外，其余值只能出现在其契约允许的帧类型中（例如
    ``LINK_LOST``/``WATCHDOG_EXPIRED`` 仅故障遥测、``STOP_REJECTED`` 仅失败
    STOP_ACK、``DUPLICATE_FRAME``/``MALFORMED_FRAME`` 仅失败普通 ACK）。
    ``ACK_TIMEOUT``/``STOP_TIMEOUT`` 绝不来自 MCU，仅主机诊断使用。
    """

    NONE = 0
    ACK_TIMEOUT = 1
    STOP_TIMEOUT = 2
    STOP_REJECTED = 3
    LINK_LOST = 4
    DUPLICATE_FRAME = 5
    WATCHDOG_EXPIRED = 6
    MALFORMED_FRAME = 7


class _WireMode(IntEnum):
    """设备模式注册表：idle/moving/holding/stopped/faulted（0x00..0x04）。"""

    IDLE = 0
    MOVING = 1
    HOLDING = 2
    STOPPED = 3
    FAULTED = 4


# 仲裁标识符 -> 帧类型的一一映射。decode_can_frame 以此判定帧类型，
# 未列出的标准标识符一律被拒绝（mcu-wire-v1.md：ID 恰好选择一种帧类型）。
_ID_TO_KIND = {
    MCU_CAN_ID_COMMAND: CanFrameKind.COMMAND,
    MCU_CAN_ID_ACK: CanFrameKind.ACK,
    MCU_CAN_ID_TELEMETRY: CanFrameKind.TELEMETRY,
    MCU_CAN_ID_STOP: CanFrameKind.STOP,
    MCU_CAN_ID_STOP_ACK: CanFrameKind.STOP_ACK,
}
# 普通命令 opcode 白名单：除 STOP 之外的全部 Wire V1 opcode。
# COMMAND 帧只接受 0x0000..0x7fff 的 command_id 且 opcode 在此集合内。
_ORDINARY_OPCODES = frozenset(
    {
        _WireOpcode.MOVE,
        _WireOpcode.GRIP_OPEN,
        _WireOpcode.GRIP_CLOSE,
        _WireOpcode.HOLD,
        _WireOpcode.HEARTBEAT,
    }
)
# 入站方向白名单：ACK、STOP_ACK 与遥测是仅有的入站类型。
# 出站方向白名单：COMMAND 与 STOP 是仅有的出站类型（mcu-wire-v1.md）。
_INBOUND_KINDS = frozenset({CanFrameKind.ACK, CanFrameKind.STOP_ACK, CanFrameKind.TELEMETRY})
_OUTBOUND_KINDS = frozenset({CanFrameKind.COMMAND, CanFrameKind.STOP})


class CanFrameValidationError(ValueError):
    """Wire V1 帧校验失败；该帧被整体拒绝、绝不发布输出。"""

    pass


class CanTransportError(Exception):
    """注入式传输端口暴露的基础异常。"""


class CanTransportFrameError(CanTransportError):
    """收到的传输记录格式不合法，但链路可能仍可用。"""


class CanTransportBackpressureError(CanTransportError):
    """内核传输因已满而暂时拒绝了一个帧。"""


class CanBusOffError(CanTransportError):
    """内核报告 CAN 控制器 bus-off（错误帧的 ``_CAN_ERR_BUSOFF`` 位）。

    适配器进入 ``BUS_OFF`` 并清空挂起工作；恢复需外部完成 CAN 核心重启
    后由 ``recover()`` 确认（robot-bsp-can-v0.1.md 故障行为）。
    """

    pass


class CanLinkLostError(CanTransportError):
    """链路丢失或协议级失败（STOP 拒绝/超时、故障遥测、时钟故障等）。

    失败即拒绝：普通命令被禁用，直到显式恢复成功。
    """

    pass


@dataclass(frozen=True)
class CanFrame:
    """一条不可变的 Classic CAN 帧，位于传输边界与 Wire V1 校验之间。

    ``arbitration_id`` 只携带剥离标志位后的 11/29 位 ID；``raw_can_id``
    可选地保留 Linux ``struct can_frame.can_id`` 原始值（含 EFF/RTR/ERR
    高位标志）。时间戳字段为入口观测元数据：``kernel_timestamp_ns`` 来自
    ``SO_TIMESTAMPNS``，``kernel_drop_count`` 来自 ``SO_RXQ_OVFL``，
    ``observed_*`` 为提供时的主机观测时钟（由 SocketCANTransport 填充）。
    """

    arbitration_id: int
    data: bytes
    # extended（29 位）ID 标志；Wire V1 只接受标准 11 位帧，该标志必须为 False。
    is_extended_id: bool = False
    # 远程传输请求帧；Wire V1 拒绝 RTR 帧（远程帧无数据载荷）。
    is_remote_frame: bool = False
    # CAN 错误帧；错误帧绝不进入 Wire V1 解码，bus-off 位单独检查。
    is_error_frame: bool = False
    # 数据长度码：Classic CAN 为 0..8；Wire V1 要求恰为 8。
    dlc: int | None = None
    # 内核 SO_TIMESTAMPNS 观测（纳秒）；由入口层提供，可为 None。
    kernel_timestamp_ns: int | None = None
    # 内核 SO_RXQ_OVFL 接收溢出计数器（32 位无符号）；由入口层提供。
    kernel_drop_count: int | None = None
    # 提供时的主机单调时钟观测；非有限或倒退值在入口处失败即拒绝。
    observed_monotonic_ts: float | None = None
    # 提供时的主机墙上时钟观测；与单调观测同样校验。
    observed_wall_ts: float | None = None
    # Linux 原始 can_id（含 EFF/RTR/ERR 标志位）；存在时必须与字段标志一致。
    raw_can_id: int | None = None

    @property
    def effective_dlc(self) -> int:
        """返回线缆 DLC，包括 RTR 帧无数据时的长度。"""

        return len(self.data) if self.dlc is None else self.dlc


@dataclass(frozen=True)
class CanWireFrame:
    """通过校验的 Wire V1 帧及其解码后的协议字段。

    只有通过 :func:`decode_can_frame` 完整跨字段校验的帧才被构造为本类型；
    字段按 mcu-wire-v1.md 载荷布局解码，多字节整数均为网络字节序（大端），
    与帧类型无关的字段为 None（例如遥测没有 command_id/opcode/retry_count）。
    """

    frame: CanFrame
    kind: CanFrameKind
    # COMMAND/STOP/ACK/STOP_ACK 载荷字节 1..2，无符号 16 位大端。
    command_id: int | None = None
    # 遥测载荷字节 1..4，无符号 32 位大端。
    sequence_no: int | None = None
    # 载荷字节 3 的 opcode。
    opcode: int | None = None
    # 载荷字节 4 的线上重试计数（重试时递增，ACK 回显）。
    retry_count: int | None = None
    # ACK/STOP_ACK 载荷字节 5 的结果码（_WireResult）。
    result_code: int | None = None
    # 载荷字节 6 的故障码（ACK/STOP_ACK）或遥测字节 5（_WireFault）。
    fault_code: int | None = None
    # ACK/STOP_ACK 载荷字节 7 或遥测字节 6 的设备模式（_WireMode）。
    device_mode: int | None = None


@dataclass(frozen=True)
class CanTransportConfig:
    """固定所有受限平面与关联窗口容量的配置（host-can-transport-v1.md）。

    命令、遥测、健康、外部投影、订阅者与关联容量在此固定；队列饱和返回
    类型化背压并递增有界丢弃计数。``__post_init__`` 校验全部数值范围，
    非法值立即抛出 ``ValueError``（失败即拒绝：绝不静默归一化配置）。
    """

    # 运行时命令平面容量（命令队列满时普通命令被拒绝，STOP 优先并抢占）。
    queue_capacity: int = 64
    # 普通命令的确认期限：本地分发开始计时，超时后按预算重试。
    ack_timeout_s: float = 0.100
    # 普通命令的确认重试预算：耗尽后清空普通流量并升级为关联 STOP。
    ack_retry_budget: int = 2
    # STOP 确认期限与重试预算；STOP 超时/被拒绝即失败关闭链路。
    stop_timeout_s: float = 0.050
    stop_retry_budget: int = 1
    # 运行时 worker 的轮询间隔。
    poll_interval_s: float = 0.010
    # shutdown 时等待唯一 worker join 的期限；join 超时让清理保持挂起、可重试。
    shutdown_timeout_s: float = 1.0
    # 每个仲裁标识符的最大订阅者数（订阅者注册表归运行时所有）。
    max_subscribers_per_id: int = 16
    # 健康平面（诊断）容量；最旧记录淘汰并计入健康丢弃计数。
    diagnostic_capacity: int = 128
    # 命令关联窗口容量：completed/timed_out 各有界保留，最旧淘汰。
    correlation_capacity: int = 128
    # 自动生成 STOP 帧的起始 command_id（STOP 分区 0x8000..0xffff）。
    initial_stop_command_id: int = 0x8000
    # 遥测平面容量：重复/过期帧在分发前被拒绝，饱和时最旧淘汰并计数。
    telemetry_capacity: int = 64
    # 显式健康容量（见 SafeCANBus.__init__ 与 diagnostic_capacity 的历史关系）。
    health_capacity: int = 128
    # 入口信封/外部投影的来源与接口身份标签。
    source: str = "mcu-can"
    interface: str = "injected-can"
    # 内核传输背压（ENOBUFS/EAGAIN）预算：超额即失败关闭链路。
    transport_backpressure_budget: int = 2
    # 外部投影平面容量：不可变只读记录，最旧记录淘汰并计数。
    external_capacity: int = 64

    def __post_init__(self) -> None:
        # 各容量必须为正整数；重试预算必须落在线上字节 0..255 可表示的范围。
        for name in (
            "queue_capacity",
            "telemetry_capacity",
            "health_capacity",
            "external_capacity",
            "max_subscribers_per_id",
            "correlation_capacity",
        ):
            value = getattr(self, name)
            if type(value) is not int or value <= 0:
                raise ValueError(f"{name} must be a positive integer")
        for name in ("ack_retry_budget", "stop_retry_budget", "transport_backpressure_budget"):
            value = getattr(self, name)
            if type(value) is not int or not 0 <= value <= 255:
                raise ValueError(f"{name} must be an integer from 0 through 255")
        # 期限必须为有限正数；bool 是 int 子类，须显式排除。
        for name in ("ack_timeout_s", "stop_timeout_s", "poll_interval_s", "shutdown_timeout_s"):
            value = getattr(self, name)
            if isinstance(value, bool) or not isinstance(value, int | float):
                raise ValueError(f"{name} must be a finite positive number")
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} must be a finite positive number")
        # STOP command_id 必须落在 STOP 分区（mcu-wire-v1.md：0x8000..0xffff）。
        if type(self.initial_stop_command_id) is not int or not 0x8000 <= self.initial_stop_command_id <= 0xFFFF:
            raise ValueError("initial_stop_command_id must be in the STOP command partition")
        if type(self.diagnostic_capacity) is not int or self.diagnostic_capacity <= 0:
            raise ValueError("diagnostic_capacity must be a positive integer")
        for name in ("source", "interface"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{name} must be a non-empty string")


@dataclass(frozen=True)
class CanSendResult:
    """出站准入的类型化结果（不可变）。"""

    status: CanSendStatus
    # 拒绝原因的可读描述；QUEUED 时为 None。
    reason: str | None = None
    # 关联的命令 ID（仅 COMMAND/STOP 有值）。
    command_id: int | None = None

    @property
    def accepted(self) -> bool:
        """True 当且仅当命令已进入命令平面（绝不等于 MCU 已接受）。"""

        return self.status is CanSendStatus.QUEUED


@dataclass(frozen=True)
class CanReceiveResult:
    """一次入口处理的类型化结果（不可变）。"""

    status: CanReceiveStatus
    # 通过校验的线上帧（INVALID_FRAME 可能为 None）。
    wire_frame: CanWireFrame | None = None
    # 仅接受的 ACK/STOP_ACK 且 MCU 结果为 accepted 时为 True；遥测从不确认命令。
    confirmed: bool = False
    # 订阅者回调抛异常的数量（异常被隔离并计入健康平面）。
    callback_errors: int = 0
    # 拒绝原因；ACCEPTED 时为 None。
    reason: str | None = None
    # 不可变入口元数据信封（有效入站结果携带）。
    envelope: CanTransportEnvelope | None = None
    # 面向外部观察者的只读投影（可暴露的才发布进外部队列）。
    external_record: CanExternalRecord | None = None


@dataclass(frozen=True)
class CanDiagnostic:
    """健康平面上的一条诊断记录（最旧淘汰、计数累计）。"""

    code: CanDiagnosticCode
    # 记录时的单调时钟观测（时钟故障时回退到 time.monotonic，见 _safe_monotonic_locked）。
    observed_at: float
    detail: str
    command_id: int | None = None


@dataclass(frozen=True)
class CanTransportEnvelope:
    """面向未来带类型运行时边界的不可变入口元数据。

    每个有效入站结果携带本信封：``source``/``interface`` 标识入口来源，
    ``sequence`` 是适配器入口序号（绝不因链路恢复重置，与 MCU 遥测序号
    的 Wire V1 半区间排序相互独立），``monotonic_ts``/``wall_ts`` 为入口
    观测时钟，``health`` 为该帧处理前的链路健康快照，``wire_frame`` 为
    经过校验的线上帧，``evidence_refs`` 是不可变证据引用（can-ingress://
    URI）。非有限或倒退的时钟观测是可观测的失败即拒绝入口错误。
    """

    source: str
    interface: str
    sequence: int
    monotonic_ts: float
    wall_ts: float
    health: CanLinkState
    wire_frame: CanWireFrame
    evidence_refs: tuple[str, ...] = ()
    # 内核 SO_TIMESTAMPNS 纳秒时间戳（提供时）。
    kernel_timestamp_ns: int | None = None
    # 内核 SO_RXQ_OVFL 接收溢出计数器（提供时）。
    kernel_drop_count: int | None = None
    # 时间戳来源：host/kernel/kernel+host/adapter（见 _observe_ingress_locked）。
    timestamp_source: str = "host"


@dataclass(frozen=True)
class CanExternalRecord:
    """对外部观察者安全的不可变只读投影。

    该投影有意只包含经过校验的标量帧字段与不可变字节。它从不携带 socket、
    设备句柄、回调或可写的协议对象。``frame_valid`` 表示完整的 Wire V1
    校验已通过；``exposure_allowed`` 仅在入站事件被接受时为真。
    """

    status: CanReceiveStatus
    source: str
    interface: str
    # 适配器入口序号：绝不因链路恢复重置（见 CanTransportEnvelope.sequence）。
    ingress_sequence: int
    health: CanLinkState
    frame_valid: bool
    exposure_allowed: bool
    # 外部事件类型：telemetry 或 action_result；拒绝/非入站帧为 None。
    event_type: str | None = None
    frame_kind: CanFrameKind | None = None
    # 帧元数据快照：全部为不可变标量或 bytes，绝不携带 socket/句柄/回调。
    arbitration_id: int | None = None
    raw_can_id: int | None = None
    dlc: int | None = None
    data: bytes | None = None
    is_extended_id: bool | None = None
    is_remote_frame: bool | None = None
    is_error_frame: bool | None = None
    monotonic_ts: float | None = None
    wall_ts: float | None = None
    kernel_timestamp_ns: int | None = None
    kernel_drop_count: int | None = None
    timestamp_source: str = "none"
    reason: str | None = None
    callback_errors: int = 0
    confirmed: bool = False
    # 解码后的协议字段快照（按帧类型部分填充，其余为 None）。
    command_id: int | None = None
    sequence_no: int | None = None
    opcode: int | None = None
    retry_count: int | None = None
    result_code: int | None = None
    fault_code: int | None = None
    device_mode: int | None = None
    evidence_refs: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        # 构造即校验：外部投影不允许携带矛盾或不可变之外的字段。
        if not isinstance(self.status, CanReceiveStatus):
            raise TypeError("external record status must be CanReceiveStatus")
        if not isinstance(self.health, CanLinkState):
            raise TypeError("external record health must be CanLinkState")
        if type(self.ingress_sequence) is not int or self.ingress_sequence < 0:
            raise ValueError("external record ingress_sequence must be a non-negative integer")
        if type(self.frame_valid) is not bool or type(self.exposure_allowed) is not bool:
            raise TypeError("external record validity and exposure flags must be bool")
        for name in ("source", "interface"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"external record {name} must be non-empty")
        if self.data is not None and not isinstance(self.data, bytes):
            raise TypeError("external record data must be immutable bytes")
        if self.dlc is not None and (type(self.dlc) is not int or not 0 <= self.dlc <= 8):
            raise ValueError("external record dlc must be from 0 through 8")
        if self.data is not None and self.dlc is not None and len(self.data) > self.dlc:
            raise ValueError("external record data cannot exceed its dlc")
        for name in ("is_extended_id", "is_remote_frame", "is_error_frame"):
            value = getattr(self, name)
            if value is not None and type(value) is not bool:
                raise TypeError(f"external record {name} must be a bool when present")
        if self.frame_kind is not None and not isinstance(self.frame_kind, CanFrameKind):
            raise TypeError("external record frame_kind must be CanFrameKind when present")
        if self.event_type is not None and self.event_type not in {"telemetry", "action_result"}:
            raise ValueError("external record event_type is invalid")
        if self.kernel_drop_count is not None and (
            type(self.kernel_drop_count) is not int or not 0 <= self.kernel_drop_count <= 0xFFFFFFFF
        ):
            raise ValueError("external record kernel_drop_count must be a 32-bit unsigned integer")
        for name, value in (("monotonic_ts", self.monotonic_ts), ("wall_ts", self.wall_ts)):
            if value is not None and (
                isinstance(value, bool) or not isinstance(value, int | float) or not math.isfinite(value)
            ):
                raise ValueError(f"external record {name} must be finite when present")
        if self.kernel_timestamp_ns is not None and (
            type(self.kernel_timestamp_ns) is not int or self.kernel_timestamp_ns < 0
        ):
            raise ValueError("external record kernel_timestamp_ns must be non-negative")
        if self.timestamp_source not in {"none", "adapter", "host", "kernel", "kernel+host"}:
            raise ValueError("external record timestamp_source is invalid")
        if type(self.callback_errors) is not int or self.callback_errors < 0:
            raise ValueError("external record callback_errors must be a non-negative integer")
        if not isinstance(self.confirmed, bool):
            raise TypeError("external record confirmed must be a bool")
        if self.exposure_allowed and (not self.frame_valid or self.status is not CanReceiveStatus.ACCEPTED):
            raise ValueError("external record exposure requires an accepted, valid frame")
        if self.confirmed and not self.exposure_allowed:
            raise ValueError("external record confirmation requires exposure")
        if self.confirmed and self.status is not CanReceiveStatus.ACCEPTED:
            raise ValueError("external record confirmation requires accepted status")
        for name in (
            "arbitration_id",
            "raw_can_id",
            "command_id",
            "sequence_no",
            "opcode",
            "retry_count",
            "result_code",
            "fault_code",
            "device_mode",
        ):
            value = getattr(self, name)
            if value is not None and (type(value) is not int or value < 0):
                raise ValueError(f"external record {name} must be a non-negative integer when present")
        if self.raw_can_id is not None:
            # raw_can_id 必须与仲裁标识符加 EFF/RTR/ERR 高位标志一致，否则拒绝。
            if self.arbitration_id is None or any(
                value is None for value in (self.is_extended_id, self.is_remote_frame, self.is_error_frame)
            ):
                raise ValueError("external record raw_can_id requires complete frame identity metadata")
            flags = 0
            if self.is_extended_id:
                flags |= _CAN_EFF_FLAG
            if self.is_remote_frame:
                flags |= _CAN_RTR_FLAG
            if self.is_error_frame:
                flags |= _CAN_ERR_FLAG
            if self.raw_can_id != self.arbitration_id | flags:
                raise ValueError("external record raw_can_id does not match frame flags")
        if not isinstance(self.evidence_refs, tuple) or any(
            not isinstance(reference, str) or not reference.strip() for reference in self.evidence_refs
        ):
            raise ValueError("external record evidence_refs must be a tuple of non-empty strings")

    @property
    def data_hex(self) -> str | None:
        return None if self.data is None else self.data.hex()

    def to_dict(self) -> dict[str, object | None]:
        """只为 JSON 消费方序列化安全的投影字段。"""

        return {
            "status": self.status.value,
            "source": self.source,
            "interface": self.interface,
            "ingress_sequence": self.ingress_sequence,
            "health": self.health.value,
            "frame_valid": self.frame_valid,
            "exposure_allowed": self.exposure_allowed,
            "event_type": self.event_type,
            "frame_kind": None if self.frame_kind is None else self.frame_kind.value,
            "arbitration_id": self.arbitration_id,
            "raw_can_id": self.raw_can_id,
            "dlc": self.dlc,
            "data_hex": self.data_hex,
            "is_extended_id": self.is_extended_id,
            "is_remote_frame": self.is_remote_frame,
            "is_error_frame": self.is_error_frame,
            "monotonic_ts": self.monotonic_ts,
            "wall_ts": self.wall_ts,
            "kernel_timestamp_ns": self.kernel_timestamp_ns,
            "kernel_drop_count": self.kernel_drop_count,
            "timestamp_source": self.timestamp_source,
            "reason": self.reason,
            "callback_errors": self.callback_errors,
            "confirmed": self.confirmed,
            "command_id": self.command_id,
            "sequence_no": self.sequence_no,
            "opcode": self.opcode,
            "retry_count": self.retry_count,
            "result_code": self.result_code,
            "fault_code": self.fault_code,
            "device_mode": self.device_mode,
            "evidence_refs": list(self.evidence_refs),
        }


class MonotonicClock(Protocol):
    """单调时钟协议：返回有限浮点秒数；异常由调用方按失败即拒绝处理。"""

    def monotonic(self) -> float: ...


class CanTransportPort(Protocol):
    """注入式阻塞传输端口（host-can-transport-v1.md 的「端口」角色）。

    参考实现为 ``workbench.hardware.socketcan_transport.SocketCANTransport``；
    测试中用 FakeTransport 演练。适配器不拥有端口生命周期：打开/关闭由
    ``activate``/``deactivate`` 委托，``recover`` 返回外部完成的恢复是否
    已确认。端口无 worker、无队列。
    """

    def open(self) -> None: ...

    def send(self, frame: CanFrame) -> None: ...

    def receive(self, timeout_s: float) -> CanFrame | None: ...

    def recover(self) -> bool: ...

    def close(self) -> None: ...


class _SystemClock:
    """默认单调时钟实现：直接委托标准库 time.monotonic()。"""

    def monotonic(self) -> float:
        return time.monotonic()


@dataclass
class _PendingRequest:
    """在途请求（普通命令或 STOP）的关联状态（非冻结，仅在运行时锁内变更）。

    ``deadline`` 为下一次确认期限；``retry_budget_used`` 记录已消耗的线上
    重试次数；``sent_retry_counts`` 记录本次关联下已派发的全部 retry_count
    （MCU 回显任一者即可关联，重试保持命令关联）。
    """

    wire_frame: CanWireFrame
    deadline: float
    retry_budget_used: int
    sent_retry_counts: set[int]


@dataclass(frozen=True)
class _Dispatch:
    """即将交给传输端口的一帧及其重试标记（不可变）。"""

    wire_frame: CanWireFrame
    is_retry: bool


@dataclass(frozen=True)
class _IngressObservation:
    """一次入口观测的时钟/序号快照（不可变）。

    ``valid`` 为 False 时时钟观测失败（非有限或倒退），入口帧按失败即拒绝
    处理；``reason`` 记录具体原因，绝不静默归一化。
    """

    sequence: int
    monotonic_ts: float | None
    wall_ts: float | None
    kernel_timestamp_ns: int | None
    kernel_drop_count: int | None
    timestamp_source: str
    valid: bool
    reason: str | None = None


# 关联键：(响应帧类型, command_id, opcode)。命令与 STOP 使用各自分区，绝不混用。
CorrelationKey = tuple[CanFrameKind, int, int]
# 传输背压键：(帧类型, command_id, retry_count)；相同键连续失败计入背压预算。
TransportAttemptKey = tuple[CanFrameKind, int | None, int | None]
# 订阅者回调：接收 CanWireFrame；异常由运行时隔离并计入健康平面。
Subscriber = Callable[[CanWireFrame], None]


class DeviceAdapter(Protocol):
    """由 ``DeviceRuntime`` 托管的硬件适配器所实现的端口。"""

    def configure(self) -> bool: ...

    def activate(self) -> bool: ...

    def poll(self, receive_timeout_s: float) -> object | None: ...

    def deactivate(self) -> bool: ...

    def cleanup(self) -> bool: ...


class DeviceRuntime:
    """单个设备的唯一生命周期、worker、队列与回调所有者。

    运行时在其公共边界处与传输无关。CAN 适配器在持有同一把锁时使用私有的
    命令/遥测/健康平面方法；它从不创建另一个 worker、取消令牌、生命周期
    状态机、队列或订阅者注册表。
    """

    def __init__(
        self,
        adapter: DeviceAdapter,
        *,
        command_capacity: int,
        telemetry_capacity: int,
        health_capacity: int,
        max_subscribers_per_id: int,
        poll_interval_s: float,
        external_capacity: int | None = None,
    ) -> None:
        self._validate_capacity(command_capacity, "command_capacity")
        self._validate_capacity(telemetry_capacity, "telemetry_capacity")
        self._validate_capacity(health_capacity, "health_capacity")
        self._validate_capacity(max_subscribers_per_id, "max_subscribers_per_id")
        if external_capacity is None:
            external_capacity = telemetry_capacity
        self._validate_capacity(external_capacity, "external_capacity")
        if (
            isinstance(poll_interval_s, bool)
            or not isinstance(poll_interval_s, int | float)
            or not math.isfinite(poll_interval_s)
            or poll_interval_s <= 0
        ):
            raise ValueError("poll_interval_s must be a finite positive number")

        self._adapter = adapter
        self._command_capacity = command_capacity
        self._telemetry_capacity = telemetry_capacity
        self._health_capacity = health_capacity
        self._external_capacity = external_capacity
        self._max_subscribers_per_id = max_subscribers_per_id
        self._poll_interval_s = float(poll_interval_s)
        self._lock = threading.RLock()
        self._cancel_event = threading.Event()
        self._state = DeviceRuntimeState.NEW
        self._worker: threading.Thread | None = None
        self._lifecycle_operation: str | None = None
        self._active_operations = 0
        self._generation = 0
        self._configured = False
        self._activated = False
        self._cleanup_in_progress = False
        self._cleanup_done = False
        self._shutdown_requested = False
        self._shutdown_result: bool | None = None
        self._shutdown_timeout_reported = False

        # 这些是仅有的运行时数据平面。适配器的协议状态由同一把锁保护，
        # 因此准入与派发对生命周期转换而言是原子的。
        self._command_queue: deque[object] = deque()
        self._telemetry_queue: deque[object] = deque()
        self._health_queue: deque[object] = deque()
        self._external_queue: deque[object] = deque()
        self._telemetry_drop_count = 0
        self._health_drop_count = 0
        self._external_drop_count = 0
        self._error_count = 0
        self._subscribers: dict[int, list[Subscriber]] = {}

    @staticmethod
    def _validate_capacity(value: int, name: str) -> None:
        """拒绝非正整数的容量参数（失败即拒绝：绝不静默钳位）。"""

        if type(value) is not int or value <= 0:
            raise ValueError(f"{name} must be a positive integer")

    @property
    def lock(self) -> threading.RLock:
        """适配器端口使用的共享状态锁。"""

        return self._lock

    @property
    def state(self) -> DeviceRuntimeState:
        """运行时生命周期状态快照（锁内读取）。"""

        with self._lock:
            return self._state

    @property
    def command_depth(self) -> int:
        """命令平面当前深度（锁内读取）。"""

        with self._lock:
            return len(self._command_queue)

    @property
    def telemetry_depth(self) -> int:
        """遥测平面当前深度（锁内读取）。"""

        with self._lock:
            return len(self._telemetry_queue)

    @property
    def telemetry_drop_count(self) -> int:
        """遥测平面因饱和淘汰最旧记录的累计次数。"""

        with self._lock:
            return self._telemetry_drop_count

    @property
    def health_drop_count(self) -> int:
        """健康平面因饱和淘汰最旧记录的累计次数。"""

        with self._lock:
            return self._health_drop_count

    @property
    def external_depth(self) -> int:
        """外部投影平面当前深度（锁内读取）。"""

        with self._lock:
            return len(self._external_queue)

    @property
    def external_drop_count(self) -> int:
        """外部投影平面因饱和淘汰最旧记录的累计次数。"""

        with self._lock:
            return self._external_drop_count

    @property
    def error_count(self) -> int:
        """健康平面累计错误计数（含因容量被淘汰的记录）。"""

        with self._lock:
            return self._error_count

    @property
    def worker_alive(self) -> bool:
        """唯一 worker 是否存在且存活（锁内读取）。"""

        with self._lock:
            return self._worker is not None and self._worker.is_alive()

    def start(self, *, background: bool = True) -> bool:
        """执行一次 configure/activate，并可选地启动唯一的 worker。"""

        if type(background) is not bool:
            raise ValueError("background must be a bool")
        with self._lock:
            # 仅允许从 NEW 启动一次；重复 start 返回 False。
            if self._state is not DeviceRuntimeState.NEW:
                return False
            self._state = DeviceRuntimeState.CONFIGURING
            self._lifecycle_operation = "start"
            self._generation += 1
            self._cancel_event.clear()

        try:
            # configure 在锁外执行：适配器可能阻塞；异常按失败即拒绝隔离。
            configured = bool(self._adapter.configure())
        except Exception as exc:  # noqa: BLE001 - 适配器故障在运行时边界处被隔离。
            self._notify_adapter_error(exc)
            configured = False

        with self._lock:
            if configured:
                self._configured = True
            if not configured:
                if self._state is not DeviceRuntimeState.DEACTIVATING:
                    self._state = DeviceRuntimeState.FAILED
            elif self._state is DeviceRuntimeState.CONFIGURING:
                self._state = DeviceRuntimeState.CONFIGURED
            else:
                # shutdown() 赢得了竞争；不要激活被放弃的端口。
                configured = False

        if not configured:
            self._finish_start(False)
            return False

        with self._lock:
            # 只有仍处于 CONFIGURED 才允许激活；否则放弃本次启动。
            if self._state is not DeviceRuntimeState.CONFIGURED:
                activate = False
            else:
                self._state = DeviceRuntimeState.ACTIVATING
                activate = True
        if not activate:
            self._finish_start(False)
            return False

        try:
            # activate 同样在锁外执行（打开端口可能阻塞）；异常被隔离。
            activated = bool(self._adapter.activate())
        except Exception as exc:  # noqa: BLE001 - 适配器故障在运行时边界处被隔离。
            self._notify_adapter_error(exc)
            activated = False

        with self._lock:
            # 激活被接受的条件：适配器成功、状态仍为 ACTIVATING、且关闭未请求。
            self._activated = activated
            accepted = activated and self._state is DeviceRuntimeState.ACTIVATING and not self._shutdown_requested
            if accepted:
                self._state = DeviceRuntimeState.ACTIVE
                self._lifecycle_operation = None
                if background:
                    # 唯一的 I/O worker：守护线程，由 shutdown 取消并 join。
                    self._worker = threading.Thread(
                        target=self._worker_main,
                        name="device-runtime-worker",
                        daemon=True,
                    )
                    self._worker.start()
                    return True
                return True

        self._finish_start(activated)
        return False

    def _finish_start(self, activated: bool) -> None:
        """处置一个失败或在 shutdown 竞争中落败的激活。"""

        with self._lock:
            shutdown_requested = self._shutdown_requested or self._state is DeviceRuntimeState.DEACTIVATING
            self._state = DeviceRuntimeState.DEACTIVATING
            # 保留生命周期标记，直到所有者完成清理。
            # 并发 shutdown 不得把这个交接窗口误认为空闲运行时，
            # 从而启动第二个清理所有者。
            self._cleanup_in_progress = True
        cleanup_ok = self._run_cleanup(activated)
        with self._lock:
            self._cleanup_in_progress = False
            self._cleanup_done = cleanup_ok
            self._activated = False
            self._worker = None
            self._lifecycle_operation = None
            if shutdown_requested:
                self._state = DeviceRuntimeState.CLEANED if cleanup_ok else DeviceRuntimeState.FAILED
                self._shutdown_result = cleanup_ok
            else:
                self._state = DeviceRuntimeState.FAILED

    def service_once(self, *, receive_timeout_s: float = 0.0) -> object | None:
        """让运行时唯一的 worker（或手动调用方）轮询适配器。"""

        if (
            isinstance(receive_timeout_s, bool)
            or not isinstance(receive_timeout_s, int | float)
            or not math.isfinite(receive_timeout_s)
            or receive_timeout_s < 0
        ):
            raise ValueError("receive_timeout_s must be a finite non-negative number")
        with self._lock:
            if self._state is not DeviceRuntimeState.ACTIVE:
                return None
            if self._worker is not None and threading.current_thread() is not self._worker:
                raise RuntimeError("manual service is unavailable while the background worker is active")
        try:
            return self._adapter.poll(float(receive_timeout_s))
        except Exception as exc:  # noqa: BLE001 - 适配器异常按失败即拒绝处理，且不会终止进程。
            self._notify_adapter_error(exc)
            return None

    def shutdown(self, *, timeout_s: float) -> bool:
        """恰好一次地取消、join 并清理。

        ``False`` 表示仍有 worker 或清理操作未完成；调用方可以重试。
        处于终态 ``CLEANED`` 的运行时返回保存的清理结果，而不是把状态标签当作成功。
        """

        if (
            isinstance(timeout_s, bool)
            or not isinstance(timeout_s, int | float)
            or not math.isfinite(timeout_s)
            or timeout_s < 0
        ):
            raise ValueError("timeout_s must be a finite non-negative number")
        deadline = time.monotonic() + float(timeout_s)
        with self._lock:
            # 终态 CLEANED 返回保存的清理结果，而不是把状态标签当作成功。
            if self._state is DeviceRuntimeState.CLEANED:
                return self._shutdown_result is True
            self._shutdown_requested = True
            self._cancel_event.set()
            # 清空排队工作：先取消生产者、拒绝新命令、清空排队工作。
            self._command_queue.clear()
            self._telemetry_queue.clear()
            self._external_queue.clear()
            if self._state is not DeviceRuntimeState.CLEANED:
                self._state = DeviceRuntimeState.DEACTIVATING
            worker = self._worker

            # 适配器可能正处于阻塞的 configure/activate/recovery 操作中。
            # 在返回之前，该调用归它所有；清理不得与之竞争。
            # 该操作的完成路径会调用终结器。

        shutdown_hook = getattr(self._adapter, "on_runtime_shutdown_requested", None)
        if shutdown_hook is not None:
            shutdown_hook()

        if worker is not None and worker is not threading.current_thread():
            # 按剩余期限 join 唯一 worker；超时则清理保持挂起并返回 False，可重试。
            remaining = max(0.0, deadline - time.monotonic())
            worker.join(remaining)
            if worker.is_alive():
                self._notify_shutdown_timeout()
                return False
        elif worker is threading.current_thread():
            # 绝不在 worker 自身线程内 join 自己。
            self._notify_shutdown_timeout()
            return False

        with self._lock:
            cleanup_in_progress = self._cleanup_in_progress
            operation_pending = self._lifecycle_operation is not None or self._active_operations > 0
            cleaned = self._state is DeviceRuntimeState.CLEANED
            shutdown_result = self._shutdown_result
        if cleanup_in_progress:
            return False
        if operation_pending:
            # 取消已被接受，但拥有该阻塞调用的操作尚未完成其清理。
            # 先报告失败，直到后续调用观察到 CLEANED 及其保存的结果。
            return False
        if cleaned:
            return shutdown_result is True
        result = self._finalize_shutdown()
        return result is True

    def _finalize_shutdown(self) -> bool | None:
        """执行清理终结器并保存结果。

        返回 None 表示仍有在途操作或清理进行中，调用方应稍后重试；
        False 表示前置条件不满足或清理失败；True 表示清理成功。
        """

        with self._lock:
            if self._state is DeviceRuntimeState.CLEANED:
                return self._shutdown_result
            if self._state is not DeviceRuntimeState.DEACTIVATING:
                return False
            # worker 存活、生命周期操作在途或清理进行中都不得并发终结。
            if self._worker is not None and self._worker.is_alive():
                return False
            if self._lifecycle_operation is not None or self._active_operations > 0 or self._cleanup_in_progress:
                return None
            self._cleanup_in_progress = True
            activated = self._activated
        cleanup_ok = self._run_cleanup(activated)
        with self._lock:
            self._cleanup_in_progress = False
            self._cleanup_done = cleanup_ok
            self._activated = False
            self._worker = None
            # 清理成功进入 CLEANED 并保存结果；失败进入 FAILED，不伪装成功。
            self._state = DeviceRuntimeState.CLEANED if cleanup_ok else DeviceRuntimeState.FAILED
            self._shutdown_result = cleanup_ok
        return cleanup_ok

    def _run_cleanup(self, activated: bool) -> bool:
        """deactivate + cleanup 的合并执行；异常被隔离为清理失败。"""

        deactivate_ok = True
        cleanup_ok = True
        if activated or self._configured:
            try:
                deactivate_ok = bool(self._adapter.deactivate())
            except Exception as exc:  # noqa: BLE001 - 清理必须报告失败，而不是让适配器复活。
                self._notify_adapter_error(exc)
                deactivate_ok = False
        try:
            cleanup_ok = bool(self._adapter.cleanup())
        except Exception as exc:  # noqa: BLE001 - cleanup must report failure, not revive the adapter.
            self._notify_adapter_error(exc)
            cleanup_ok = False
        return deactivate_ok and cleanup_ok

    def begin_operation(self) -> int | None:
        """为阻塞的适配器操作预留保护，防止并发清理。

        仅运行时 ACTIVE 时可调用；返回代数令牌（generation token），
        与 operation_is_current 配对使用。
        """

        with self._lock:
            if self._state is not DeviceRuntimeState.ACTIVE:
                return None
            self._active_operations += 1
            return self._generation

    def operation_is_current(self, token: int) -> bool:
        """令牌是否仍对应当前 ACTIVE 代（并发 shutdown 会使结果失效）。"""

        with self._lock:
            return self._state is DeviceRuntimeState.ACTIVE and token == self._generation

    def end_operation(self, token: int) -> None:
        """结束一个阻塞操作；若关闭已请求且无在途工作，则触发清理终结。"""

        del token
        with self._lock:
            if self._active_operations > 0:
                self._active_operations -= 1
            should_finalize = (
                self._shutdown_requested
                and self._state is DeviceRuntimeState.DEACTIVATING
                and self._active_operations == 0
                and self._lifecycle_operation is None
                and (self._worker is None or not self._worker.is_alive())
            )
        if should_finalize:
            self._finalize_shutdown()

    def submit_command(
        self,
        item: object,
        *,
        priority: bool,
        admit: Callable[[], CanSendResult],
        on_priority: Callable[[], None],
        on_backpressure: Callable[[], CanSendResult],
    ) -> CanSendResult:
        """原子地把一条适配器命令准入运行时命令平面。"""

        with self._lock:
            result = admit()
            if not result.accepted:
                return result
            if not priority and len(self._command_queue) >= self._command_capacity:
                return on_backpressure()
            if priority:
                on_priority()
            self._command_queue.appendleft(item) if priority else self._command_queue.append(item)
            return result

    def _pop_command_locked(self) -> object | None:
        """在锁内从命令平面弹出 1 条（仅锁内调用）。"""

        return self._command_queue.popleft() if self._command_queue else None

    def _command_snapshot_locked(self) -> tuple[object, ...]:
        """命令平面的不可变快照（仅锁内调用，供关联/抢占检查）。"""

        return tuple(self._command_queue)

    def _clear_commands_locked(self) -> None:
        """清空命令平面（仅锁内调用；STOP 抢占与故障恢复使用）。"""

        self._command_queue.clear()

    def _clear_external_locked(self) -> None:
        """清空外部投影平面（仅锁内调用；关闭/清理使用）。"""

        self._external_queue.clear()

    def _prepend_command_locked(self, item: object) -> None:
        """把 1 条命令放回队列头部（仅锁内调用；传输背压重试使用）。"""

        self._command_queue.appendleft(item)

    def _enqueue_priority_locked(self, item: object) -> None:
        """以优先级身份入队：先清空普通流量，STOP 优先并抢占（仅锁内调用）。"""

        self._command_queue.clear()
        self._command_queue.appendleft(item)

    def publish_telemetry(self, item: object) -> None:
        """把 1 条数据放入受限的遥测平面。

        生产者与派发器是相互独立的操作，即使 CAN 轮询路径在入口后立即
        排空 1 条数据。这使容量与丢弃策略对批量或扇入数据的适配器保持真实。
        """

        with self._lock:
            if len(self._telemetry_queue) >= self._telemetry_capacity:
                self._telemetry_queue.popleft()
                self._telemetry_drop_count += 1
            self._telemetry_queue.append(item)

    def take_telemetry(self) -> object | None:
        """从遥测平面弹出 1 条；空时返回 None（锁内操作）。"""

        with self._lock:
            return self._telemetry_queue.popleft() if self._telemetry_queue else None

    def publish_external(self, item: object) -> None:
        """把 1 条不可变外部记录发布到受限的读取平面。"""

        with self._lock:
            # 饱和时淘汰最旧记录并计数（最旧记录淘汰策略）。
            if len(self._external_queue) >= self._external_capacity:
                self._external_queue.popleft()
                self._external_drop_count += 1
            self._external_queue.append(item)

    def take_external(self) -> object | None:
        """从外部投影平面弹出 1 条；空时返回 None（锁内操作）。"""

        with self._lock:
            return self._external_queue.popleft() if self._external_queue else None

    def external_records(self) -> tuple[object, ...]:
        """外部投影平面的不可变快照（只读，不改变队列）。"""

        with self._lock:
            return tuple(self._external_queue)

    def record_health(self, item: object) -> None:
        """把 1 条记录写入健康平面并递增错误计数。"""

        with self._lock:
            self._record_health_locked(item)

    def _record_health_locked(self, item: object) -> None:
        """健康平面入队（仅锁内调用）：最旧记录淘汰、丢弃计数与错误计数。"""

        if len(self._health_queue) >= self._health_capacity:
            self._health_queue.popleft()
            self._health_drop_count += 1
        self._health_queue.append(item)
        self._error_count += 1

    def health_records(self) -> tuple[object, ...]:
        """健康平面的不可变快照（只读）。"""

        with self._lock:
            return tuple(self._health_queue)

    def subscribe(self, arbitration_id: int, handler: Subscriber) -> bool:
        """按仲裁标识符注册订阅者；容量满或重复注册返回 False。"""

        with self._lock:
            handlers = self._subscribers.setdefault(arbitration_id, [])
            if any(existing is handler for existing in handlers):
                return False
            if len(handlers) >= self._max_subscribers_per_id:
                return False
            handlers.append(handler)
            return True

    def unsubscribe(self, arbitration_id: int, handler: Subscriber) -> bool:
        """按身份移除订阅者；不存在返回 False。"""

        with self._lock:
            handlers = self._subscribers.get(arbitration_id)
            if handlers is None:
                return False
            for index, existing in enumerate(handlers):
                if existing is handler:
                    del handlers[index]
                    if not handlers:
                        del self._subscribers[arbitration_id]
                    return True
            return False

    def dispatch_callbacks(self, wire_frame: CanWireFrame) -> int:
        """派发一个加锁快照，并返回失败数量。"""

        with self._lock:
            handlers = tuple(self._subscribers.get(wire_frame.frame.arbitration_id, ()))
        callback_errors = 0
        for handler in handlers:
            try:
                handler(wire_frame)
            except Exception as exc:  # noqa: BLE001 - 回调失败不能终止运行时 worker。
                callback_errors += 1
                logger.error("CAN subscriber failed: %s", exc)
        return callback_errors

    def _worker_main(self) -> None:
        """唯一 I/O worker 的主循环。

        以 poll_interval_s 为周期调用 service_once；非 ACTIVE 立即退出；
        适配器无结果时等待一个间隔，避免在故障状态下空转 CPU（失败即拒绝
        状态保持受限，直到外部恢复）。
        """

        while not self._cancel_event.is_set():
            with self._lock:
                if self._state is not DeviceRuntimeState.ACTIVE:
                    return
            result = self.service_once(receive_timeout_s=self._poll_interval_s)
            if result is None:
                # 适配器可能处于故障状态，故意在不触碰文件描述符的情况下返回。
                # 保持该失败即拒绝状态受限，而不是在等待修复时空转 CPU。
                self._cancel_event.wait(self._poll_interval_s)

    def _notify_adapter_error(self, error: Exception) -> None:
        """把运行时边界处的适配器异常通知给适配器（on_runtime_error 钩子）。"""

        callback = getattr(self._adapter, "on_runtime_error", None)
        if callback is not None:
            callback(error)
        else:
            logger.error("device adapter failed: %s", error)

    def _notify_shutdown_timeout(self) -> None:
        """一次性通知适配器：worker 未在关闭期限内停止（join 超时）。"""

        with self._lock:
            if self._shutdown_timeout_reported:
                return
            self._shutdown_timeout_reported = True
        callback = getattr(self._adapter, "on_runtime_shutdown_timeout", None)
        if callback is not None:
            callback()


def decode_can_frame(frame: object) -> CanWireFrame:
    """校验并解码 1 个完整的 MCU CAN Wire V1 帧。

    失败即拒绝：当 ID、DLC、版本、保留字节、枚举值、ID 分区或跨字段结果
    语义无效时，在发布输出前拒绝整个帧（mcu-wire-v1.md）。返回携带解码
    协议字段的 :class:`CanWireFrame`；任何违反契约的输入抛出
    :class:`CanFrameValidationError`（``ValueError`` 子类）。权威二进制布局
    在 ``firmware/mcu/core/frame_codec.c``，本函数只复制入口前所需校验。
    """

    # 帧类型边界：非 CanFrame、非标准 11 位、extended/RTR/错误帧、非 bytes 载荷。
    if not isinstance(frame, CanFrame):
        raise CanFrameValidationError("transport frames must be CanFrame instances")
    if type(frame.arbitration_id) is not int or not 0 <= frame.arbitration_id <= 0x7FF:
        raise CanFrameValidationError("arbitration_id must be a standard 11-bit integer")
    if type(frame.is_extended_id) is not bool or frame.is_extended_id:
        raise CanFrameValidationError("extended CAN frames are not supported")
    if type(frame.is_remote_frame) is not bool or frame.is_remote_frame:
        raise CanFrameValidationError("remote CAN frames are not supported")
    if type(frame.is_error_frame) is not bool or frame.is_error_frame:
        raise CanFrameValidationError("CAN error frames are not protocol frames")
    if not isinstance(frame.data, bytes):
        raise CanFrameValidationError("CAN payload must be immutable bytes")
    # raw_can_id（若提供）必须与仲裁标识符及 EFF/RTR/ERR 标志位一致。
    _validate_raw_can_id(frame)
    if frame.dlc is not None and (type(frame.dlc) is not int or not 0 <= frame.dlc <= 8):
        raise CanFrameValidationError("CAN DLC must be an integer from 0 through 8")
    if frame.dlc is not None and frame.dlc != len(frame.data):
        raise CanFrameValidationError("CAN payload length must match DLC")
    _validate_frame_timestamps(frame)
    # 每种 Wire V1 帧类型 DLC 都恰为 8。
    if frame.effective_dlc != MCU_WIRE_DLC:
        raise CanFrameValidationError("CAN Wire V1 requires DLC 8")

    # 仲裁标识符恰好选择一种帧类型；未列出的标准标识符被拒绝。
    kind = _ID_TO_KIND.get(frame.arbitration_id)
    if kind is None:
        raise CanFrameValidationError("unknown CAN Wire V1 arbitration ID")
    data = frame.data
    # 载荷字节 0 必须是紧凑协议版本 0x10。
    if data[0] != MCU_WIRE_VERSION_V1:
        raise CanFrameValidationError("unsupported CAN Wire version")

    if kind in {CanFrameKind.COMMAND, CanFrameKind.STOP}:
        # Command/STOP 布局：0=version，1..2=command_id（大端），3=opcode，
        # 4=retry_count，5..7=保留零。
        if any(data[index] != 0 for index in (5, 6, 7)):
            raise CanFrameValidationError("command reserved bytes must be zero")
        command_id = int.from_bytes(data[1:3], "big")
        opcode = data[3]
        # 普通命令只接受 ID 0x0000..0x7fff 与普通 opcode；
        # STOP 只接受 ID 0x8000..0xffff 与 opcode stop（mcu-wire-v1.md 分区）。
        if kind is CanFrameKind.COMMAND and not (command_id <= 0x7FFF and opcode in _ORDINARY_OPCODES):
            raise CanFrameValidationError("ordinary command ID or opcode is outside its partition")
        if kind is CanFrameKind.STOP and not (command_id >= 0x8000 and opcode == _WireOpcode.STOP):
            raise CanFrameValidationError("STOP command ID or opcode is outside its partition")
        return CanWireFrame(
            frame=frame,
            kind=kind,
            command_id=command_id,
            opcode=opcode,
            retry_count=data[4],
        )

    if kind in {CanFrameKind.ACK, CanFrameKind.STOP_ACK}:
        # ACK/STOP_ACK 布局：0=version，1..2=command_id（大端），3=opcode，
        # 4=回显 retry_count，5=result_code，6=fault_code，7=device_mode。
        command_id = int.from_bytes(data[1:3], "big")
        opcode = data[3]
        retry_count = data[4]
        result_code = data[5]
        fault_code = data[6]
        device_mode = data[7]
        if kind is CanFrameKind.ACK:
            # 普通 ACK：接受时 fault 必须为 none 且模式为 idle..stopped；
            # 拒绝时仅允许 duplicate_frame/malformed_frame 且模式 faulted。
            valid_partition = command_id <= 0x7FFF and opcode in _ORDINARY_OPCODES
            valid_result = (
                result_code == _WireResult.ACCEPTED
                and fault_code == _WireFault.NONE
                and _WireMode.IDLE <= device_mode <= _WireMode.STOPPED
            ) or (
                result_code == _WireResult.REJECTED
                and fault_code in {_WireFault.DUPLICATE_FRAME, _WireFault.MALFORMED_FRAME}
                and device_mode == _WireMode.FAULTED
            )
        else:
            # STOP_ACK：接受时 fault 必须为 none 且模式 stopped；
            # 拒绝时仅允许 stop_rejected 且模式 faulted。
            valid_partition = command_id >= 0x8000 and opcode == _WireOpcode.STOP
            valid_result = (
                result_code == _WireResult.ACCEPTED
                and fault_code == _WireFault.NONE
                and device_mode == _WireMode.STOPPED
            ) or (
                result_code == _WireResult.REJECTED
                and fault_code == _WireFault.STOP_REJECTED
                and device_mode == _WireMode.FAULTED
            )
        # 仅编码一个数字枚举值是不够的：组合语义必须满足冻结逻辑协议。
        if not valid_partition or not valid_result:
            raise CanFrameValidationError("acknowledgement fields violate CAN Wire V1 semantics")
        return CanWireFrame(
            frame=frame,
            kind=kind,
            command_id=command_id,
            opcode=opcode,
            retry_count=retry_count,
            result_code=result_code,
            fault_code=fault_code,
            device_mode=device_mode,
        )

    # 遥测布局：0=version，1..4=sequence_no（大端），5=fault_code，
    # 6=device_mode，7=保留零。遥测从不确认命令。
    if data[7] != 0:
        raise CanFrameValidationError("telemetry reserved byte must be zero")
    sequence_no = int.from_bytes(data[1:5], "big")
    fault_code = data[5]
    device_mode = data[6]
    # 遥测故障仅允许 link_lost/watchdog_expired 且模式 faulted；
    # 无故障时模式必须为 idle..stopped。
    valid_telemetry = (fault_code == _WireFault.NONE and _WireMode.IDLE <= device_mode <= _WireMode.STOPPED) or (
        fault_code in {_WireFault.LINK_LOST, _WireFault.WATCHDOG_EXPIRED} and device_mode == _WireMode.FAULTED
    )
    if not valid_telemetry:
        raise CanFrameValidationError("telemetry fields violate CAN Wire V1 semantics")
    return CanWireFrame(
        frame=frame,
        kind=kind,
        sequence_no=sequence_no,
        fault_code=fault_code,
        device_mode=device_mode,
    )


def _validate_frame_timestamps(frame: CanFrame) -> None:
    """校验内核时间戳、接收溢出计数器与主机观测时钟的数值范围。

    非有限或倒退的观测会在入口处理阶段被 :meth:`SafeCANBus._observe_ingress_locked`
    拒绝；这里只保证字段类型与范围合法。
    """

    _validate_raw_can_id(frame)
    if frame.kernel_timestamp_ns is not None and (
        type(frame.kernel_timestamp_ns) is not int or frame.kernel_timestamp_ns < 0
    ):
        raise CanFrameValidationError("kernel timestamp must be a non-negative integer")
    if frame.kernel_drop_count is not None and (
        type(frame.kernel_drop_count) is not int or not 0 <= frame.kernel_drop_count <= 0xFFFFFFFF
    ):
        raise CanFrameValidationError("kernel RX drop count must be a 32-bit unsigned integer")
    for name in ("observed_monotonic_ts", "observed_wall_ts"):
        value = getattr(frame, name)
        if value is not None and (
            isinstance(value, bool) or not isinstance(value, int | float) or not math.isfinite(value)
        ):
            raise CanFrameValidationError(f"{name} must be finite when present")


def _validate_raw_can_id(frame: CanFrame) -> None:
    """在协议使用前拒绝过期或矛盾的原始 ID 元数据。"""

    raw_can_id = frame.raw_can_id
    if raw_can_id is None:
        return
    if type(raw_can_id) is not int or not 0 <= raw_can_id <= 0xFFFFFFFF:
        raise CanFrameValidationError("raw CAN ID must be a 32-bit unsigned integer when present")
    flags = 0
    if frame.is_extended_id:
        flags |= _CAN_EFF_FLAG
    if frame.is_remote_frame:
        flags |= _CAN_RTR_FLAG
    if frame.is_error_frame:
        flags |= _CAN_ERR_FLAG
    expected = frame.arbitration_id | flags
    if raw_can_id != expected:
        raise CanFrameValidationError("raw CAN ID does not match arbitration ID and frame flags")


class SafeCANBus:
    """由 1 个共享的 :class:`DeviceRuntime` 托管的 CAN ``DeviceAdapter``。

    下方的 ``CanLinkState`` 仅表示链路/协议健康。生命周期、worker、取消、
    受限平面与回调都归 ``self._runtime`` 所有。为兼容最初的 Issue #55 API，
    本外观（facade）上保留的少量 ``start``/``shutdown`` 方法会委托给该所有者。

    线程安全约束：全部协议关联状态（``_pending_command``/``_pending_stop``/
    ``_dispatching``/``_completed``/``_timed_out``/序号与时钟观测）只允许在
    ``self._runtime.lock``（一把 ``RLock``）内读写；``send``/``receive``/
    ``recover`` 等阻塞端口调用在锁外执行，与关闭竞争时通过运行时代数令牌
    （generation）判定结果是否可应用。回调在锁外派发，异常被隔离为健康记录。
    """

    def __init__(
        self,
        transport: CanTransportPort,
        *,
        clock: MonotonicClock | None = None,
        wall_clock: Callable[[], float] | None = None,
        config: CanTransportConfig | None = None,
    ) -> None:
        # 注入的阻塞传输端口：构造绝不打开发送路径或任何硬件设备。
        self._transport = transport
        # 时钟注入：测试用 FakeClock 演练超时/回绕；默认系统时钟。
        self._clock = clock or _SystemClock()
        self._wall_clock = wall_clock or time.time
        if not callable(self._wall_clock):
            raise TypeError("wall_clock must be callable")
        if config is None:
            # 未提供配置时，从端口属性继承 source/interface 身份标签。
            transport_source = getattr(transport, "source", None)
            transport_interface = getattr(transport, "interface", None)
            config_kwargs = {
                name: value
                for name, value in (("source", transport_source), ("interface", transport_interface))
                if isinstance(value, str) and value.strip()
            }
            config = CanTransportConfig(**config_kwargs)
        else:
            # 显式配置与端口身份标签矛盾即拒绝（失败即拒绝）。
            for name in ("source", "interface"):
                transport_value = getattr(transport, name, None)
                if isinstance(transport_value, str) and transport_value != getattr(config, name):
                    raise ValueError(f"transport {name} does not match CanTransportConfig {name}")
        self._config = config
        self._opened = False
        self._state = CanLinkState.NEW
        # 在途请求：普通命令与 STOP 各至多一条（有界在途窗口）。
        self._pending_command: _PendingRequest | None = None
        self._pending_stop: _PendingRequest | None = None
        # 正在交给传输端口的分发（send 在锁外，成功后才记账）。
        self._dispatching: _Dispatch | None = None
        # 内核传输背压统计：相同键的连续失败计入预算，超额失败关闭链路。
        self._transport_backpressure_key: TransportAttemptKey | None = None
        self._transport_backpressure_attempts = 0
        # 有界命令关联窗口：completed/timed_out 各保留最旧淘汰（按配置容量）。
        self._completed: dict[CorrelationKey, frozenset[int]] = {}
        self._completed_order: deque[CorrelationKey] = deque()
        self._timed_out: dict[CorrelationKey, frozenset[int]] = {}
        self._timed_out_order: deque[CorrelationKey] = deque()
        # 自动生成 STOP 帧的 command_id：0xffff 后回绕到 0x8000（STOP 分区）。
        self._next_stop_command_id = self._config.initial_stop_command_id
        # 遥测序号排序基线：Wire V1 半区间规则（见 _correlate_received_locked）。
        self._last_telemetry_sequence: int | None = None
        # 适配器入口序号：绝不因链路恢复重置。
        self._ingress_sequence = 0
        # 上一个接受的时钟观测：非有限或倒退观测失败即拒绝。
        self._last_monotonic_ts: float | None = None
        self._last_wall_ts: float | None = None
        self._last_observation: _IngressObservation | None = None
        self._runtime = DeviceRuntime(
            self,
            command_capacity=self._config.queue_capacity,
            telemetry_capacity=self._config.telemetry_capacity,
            # ``diagnostic_capacity`` 是健康平面上限的历史公开名称。
            # 当调用方提供更新的显式健康容量时，任一设置都会生效。
            health_capacity=min(self._config.health_capacity, self._config.diagnostic_capacity),
            max_subscribers_per_id=self._config.max_subscribers_per_id,
            poll_interval_s=self._config.poll_interval_s,
            external_capacity=self._config.external_capacity,
        )

    @property
    def runtime(self) -> DeviceRuntime:
        """本适配器使用的统一运行时所有者。"""

        return self._runtime

    @property
    def state(self) -> CanLinkState:
        """当前链路健康（按运行时生命周期状态投影）。

        运行时的 DEACTIVATING/CLEANED 投影为 SHUTDOWN，CONFIGURING/CONFIGURED/
        ACTIVATING 投影为 STARTING；其余返回本适配器的链路状态。
        """

        with self._runtime.lock:
            if self._runtime.state in {
                DeviceRuntimeState.DEACTIVATING,
                DeviceRuntimeState.CLEANED,
            }:
                return CanLinkState.SHUTDOWN
            if self._runtime.state in {
                DeviceRuntimeState.CONFIGURING,
                DeviceRuntimeState.CONFIGURED,
                DeviceRuntimeState.ACTIVATING,
            }:
                return CanLinkState.STARTING
            return self._state

    @property
    def running(self) -> bool:
        """运行时 ACTIVE 且链路处于 ACTIVE/STOPPING（可接受工作）时为 True。"""

        with self._runtime.lock:
            return self._runtime.state is DeviceRuntimeState.ACTIVE and self._state in {
                CanLinkState.ACTIVE,
                CanLinkState.STOPPING,
            }

    @property
    def queued_count(self) -> int:
        """命令平面深度（委托运行时）。"""

        return self._runtime.command_depth

    @property
    def external_drop_count(self) -> int:
        """外部投影平面因饱和淘汰最旧记录的累计次数（委托运行时）。"""

        return self._runtime.external_drop_count

    @property
    def external_depth(self) -> int:
        """外部投影平面当前深度（委托运行时）。"""

        return self._runtime.external_depth

    @property
    def pending_command_id(self) -> int | None:
        """当前关联在途命令 ID：STOP 优先于普通命令与正在分发的一帧。"""

        with self._runtime.lock:
            if self._pending_stop is not None:
                return self._pending_stop.wire_frame.command_id
            if self._pending_command is not None:
                return self._pending_command.wire_frame.command_id
            if self._dispatching is not None:
                return self._dispatching.wire_frame.command_id
            return None

    def start(self, *, background: bool = True) -> bool:
        """兼容外观：把启动委托给统一运行时（configure -> activate -> worker）。"""

        return self._runtime.start(background=background)

    def send(self, frame: object) -> CanSendResult:
        """校验并准入 1 条出站帧。

        帧先经 :func:`decode_can_frame` 完整校验；非出站方向（ACK/遥测等）
        返回 INVALID_FRAME。随后由运行时原子准入命令平面：STOP 以优先级
        入队并抢占普通流量，普通命令在平面满时返回类型化背压。成功返回
        QUEUED 只表示本地分发已排队，绝不表示 MCU 已接受命令。
        """

        try:
            wire_frame = decode_can_frame(frame)
        except CanFrameValidationError as exc:
            self._record_diagnostic(CanDiagnosticCode.INVALID_FRAME, str(exc))
            return CanSendResult(CanSendStatus.INVALID_FRAME, str(exc))
        if wire_frame.kind not in _OUTBOUND_KINDS:
            reason = "host transport only queues command and STOP frames"
            self._record_diagnostic(CanDiagnosticCode.INVALID_FRAME, reason, wire_frame.command_id)
            return CanSendResult(CanSendStatus.INVALID_FRAME, reason, wire_frame.command_id)

        return self._runtime.submit_command(
            wire_frame,
            priority=wire_frame.kind is CanFrameKind.STOP,
            admit=lambda: self._admit_command_locked(wire_frame),
            on_priority=lambda: self._preempt_for_stop_locked("explicit STOP preempted ordinary traffic"),
            on_backpressure=lambda: self._backpressure_locked(wire_frame),
        )

    def _admit_command_locked(self, wire_frame: CanWireFrame) -> CanSendResult:
        """命令准入检查（仅锁内调用）。

        拒绝条件按序为：运行时未 ACTIVE（NOT_RUNNING）；链路健康不允许该
        方向（LINK_UNAVAILABLE）；STOP 已挂在途（CORRELATION_CONFLICT）；
        命令 ID 仍被有界关联窗口保留（CORRELATION_CONFLICT）。全部通过
        返回 QUEUED。
        """

        runtime_state = self._runtime.state
        if runtime_state in {
            DeviceRuntimeState.NEW,
            DeviceRuntimeState.CONFIGURING,
            DeviceRuntimeState.CONFIGURED,
            DeviceRuntimeState.ACTIVATING,
            DeviceRuntimeState.DEACTIVATING,
            DeviceRuntimeState.CLEANED,
            DeviceRuntimeState.FAILED,
        }:
            return CanSendResult(CanSendStatus.NOT_RUNNING, command_id=wire_frame.command_id)
        # 普通命令只允许在 ACTIVE 链路发送；STOP 允许在 ACTIVE/STOPPING 发送。
        if wire_frame.kind is CanFrameKind.COMMAND and self._state is not CanLinkState.ACTIVE:
            return CanSendResult(CanSendStatus.LINK_UNAVAILABLE, command_id=wire_frame.command_id)
        if wire_frame.kind is CanFrameKind.STOP and self._state not in {
            CanLinkState.ACTIVE,
            CanLinkState.STOPPING,
        }:
            return CanSendResult(CanSendStatus.LINK_UNAVAILABLE, command_id=wire_frame.command_id)
        # 同一时刻只允许一条 STOP 在途（含排队与正在分发的一帧）。
        if wire_frame.kind is CanFrameKind.STOP and (
            self._pending_stop is not None
            or (self._dispatching is not None and self._dispatching.wire_frame.kind is CanFrameKind.STOP)
            or any(
                isinstance(queued, CanWireFrame) and queued.kind is CanFrameKind.STOP
                for queued in self._runtime._command_snapshot_locked()
            )
        ):
            reason = "a STOP acknowledgement is already pending"
            self._record_diagnostic_locked(CanDiagnosticCode.CORRELATION_CONFLICT, reason, wire_frame.command_id)
            return CanSendResult(CanSendStatus.CORRELATION_CONFLICT, reason, wire_frame.command_id)
        # 命令 ID 复用冲突：有界关联窗口（completed/timed_out）仍保留该 ID。
        if self._correlation_id_in_use_locked(wire_frame.kind, wire_frame.command_id):
            reason = "command ID is still retained by the bounded correlation window"
            self._record_diagnostic_locked(CanDiagnosticCode.CORRELATION_CONFLICT, reason, wire_frame.command_id)
            return CanSendResult(CanSendStatus.CORRELATION_CONFLICT, reason, wire_frame.command_id)
        return CanSendResult(CanSendStatus.QUEUED, command_id=wire_frame.command_id)

    def _backpressure_locked(self, wire_frame: CanWireFrame) -> CanSendResult:
        """命令平面饱和的类型化背压结果（仅锁内调用，含诊断记录）。"""

        reason = "outbound command plane capacity reached"
        self._record_diagnostic_locked(CanDiagnosticCode.BACKPRESSURE, reason, wire_frame.command_id)
        return CanSendResult(CanSendStatus.BACKPRESSURE, reason, wire_frame.command_id)

    def configure(self) -> bool:
        """准备协议状态；转换本身由运行时负责。"""

        with self._runtime.lock:
            return self._state is CanLinkState.NEW

    def activate(self) -> bool:
        """打开注入端口，但不创建 worker。"""

        with self._runtime.lock:
            self._state = CanLinkState.STARTING
        try:
            self._transport.open()
        except CanTransportError as exc:
            with self._runtime.lock:
                self._state = CanLinkState.LINK_LOST
                self._record_diagnostic_locked(CanDiagnosticCode.LINK_LOST, str(exc))
            return False
        with self._runtime.lock:
            self._opened = True
            self._state = (
                CanLinkState.ACTIVE if self._runtime.state is DeviceRuntimeState.ACTIVATING else CanLinkState.SHUTDOWN
            )
        return True

    def service_once(self, *, receive_timeout_s: float = 0.0) -> CanReceiveResult | None:
        """兼容外观，把轮询委托给统一运行时。"""

        result = self._runtime.service_once(receive_timeout_s=receive_timeout_s)
        return result if isinstance(result, CanReceiveResult) else None

    def poll(self, receive_timeout_s: float) -> CanReceiveResult | None:
        """轮询一个传输周期；仅由 ``DeviceRuntime`` 调用。

        流程：锁内挑选下一次分发（在途重试/超时升级/队列弹出一帧），在锁外
        执行传输 send；随后在锁外执行一次有界 receive。传输异常分别按背压、
        帧错误与链路错误处理（失败即拒绝）。
        """

        with self._runtime.lock:
            # 链路不健康或运行时未 ACTIVE 时轮询为空操作（不触碰文件描述符）。
            if self._state not in {CanLinkState.ACTIVE, CanLinkState.STOPPING}:
                return None
            dispatch = self._next_dispatch_locked(self._safe_monotonic_locked())
            if dispatch is not None:
                self._dispatching = dispatch

        if dispatch is not None:
            try:
                # send 在锁外执行：可能阻塞；成功分发才开始确认期限。
                self._transport.send(dispatch.wire_frame.frame)
            except CanTransportBackpressureError as exc:
                self._handle_transport_backpressure(exc)
                return None
            except CanTransportError as exc:
                self._handle_transport_error(exc)
                return None
            with self._runtime.lock:
                # 分发期间发生关闭/恢复，则放弃记账（不发布过期工作）。
                if self._dispatching is not dispatch or self._runtime.state is not DeviceRuntimeState.ACTIVE:
                    return None
                self._dispatching = None
                self._reset_transport_backpressure_locked()
                # 记账：写入在途请求、期限与已发送 retry_count（见 _record_dispatch_locked）。
                self._record_dispatch_locked(dispatch, self._safe_monotonic_locked())

        try:
            # 一次有界接收：非阻塞语义，超时返回 None。
            received = self._transport.receive(float(receive_timeout_s))
        except CanTransportFrameError as exc:
            return self._handle_transport_frame_error(exc)
        except CanTransportBackpressureError as exc:
            self._handle_transport_backpressure(exc)
            return None
        except CanTransportError as exc:
            self._handle_transport_error(exc)
            return None
        if received is None:
            return None
        return self._handle_received(received)

    def recover(self) -> bool:
        """确认外部完成的链路恢复（两阶段恢复的第二阶段）。

        仅 BUS_OFF/LINK_LOST 状态可恢复；恢复在锁外阻塞执行并用运行时代数
        令牌防止与并发 shutdown 竞争。成功时清空故障前命令与挂起关联状态
        （绝不回放过期流量），遥测序号基线重置，链路回到 ACTIVE。失败或
        被关闭抢先时返回 False。
        """

        with self._runtime.lock:
            if self._state not in {CanLinkState.BUS_OFF, CanLinkState.LINK_LOST}:
                return False
            token = self._runtime.begin_operation()
            if token is None:
                return False
        try:
            recovered = self._transport.recover()
        except CanTransportError as exc:
            self._handle_transport_error(exc)
            self._runtime.end_operation(token)
            return False
        if not recovered:
            self._runtime.end_operation(token)
            return False
        with self._runtime.lock:
            # 令牌仍当前且状态未被并发事件改变时才应用恢复结果。
            applied = self._runtime.operation_is_current(token) and self._state in {
                CanLinkState.BUS_OFF,
                CanLinkState.LINK_LOST,
            }
            if applied:
                # 清空故障前挂起工作与关联状态；绝不回放它们。
                self._runtime._clear_commands_locked()
                self._pending_command = None
                self._pending_stop = None
                self._dispatching = None
                self._reset_transport_backpressure_locked()
                self._last_telemetry_sequence = None
                self._state = CanLinkState.ACTIVE
        self._runtime.end_operation(token)
        return applied

    def shutdown(self, *, timeout_s: float | None = None) -> bool:
        """兼容外观：委托运行时 shutdown；默认使用配置的 shutdown_timeout_s。"""

        timeout = self._config.shutdown_timeout_s if timeout_s is None else timeout_s
        if timeout is None:
            timeout = self._config.shutdown_timeout_s
        return self._runtime.shutdown(timeout_s=timeout)

    def deactivate(self) -> bool:
        """在运行时取消并 join 其 worker 后关闭端口。"""

        with self._runtime.lock:
            # 清空全部在途协议工作后关闭端口；打开失败/已打开状态被正确记录。
            if self._runtime._shutdown_requested or self._state not in {
                CanLinkState.BUS_OFF,
                CanLinkState.LINK_LOST,
            }:
                self._state = CanLinkState.SHUTDOWN
            self._runtime._clear_commands_locked()
            self._pending_command = None
            self._pending_stop = None
            self._dispatching = None
            self._reset_transport_backpressure_locked()
            opened = self._opened
        if not opened:
            return True
        try:
            self._transport.close()
        except CanTransportError as exc:
            with self._runtime.lock:
                self._record_diagnostic_locked(CanDiagnosticCode.LINK_LOST, str(exc))
            return False
        with self._runtime.lock:
            self._opened = False
        return True

    def cleanup(self) -> bool:
        """销毁适配器数据平面，不重新打开或回放流量。"""

        with self._runtime.lock:
            self._runtime._clear_commands_locked()
            self._pending_command = None
            self._pending_stop = None
            self._dispatching = None
            self._reset_transport_backpressure_locked()
            self._last_telemetry_sequence = None
            self._ingress_sequence = 0
            self._last_monotonic_ts = None
            self._last_wall_ts = None
            self._last_observation = None
            self._runtime._telemetry_queue.clear()
            self._runtime._clear_external_locked()
            self._runtime._subscribers.clear()
            return not self._opened

    def subscribe(self, arbitration_id: int, handler: Subscriber) -> bool:
        """订阅某个已知 Wire V1 仲裁标识符的入站事件。

        只接受五种已知标识符；注册/容量管理委托运行时，回调在锁外以不可变
        快照派发，异常被隔离为健康记录。
        """

        if type(arbitration_id) is not int or arbitration_id not in _ID_TO_KIND:
            raise ValueError("subscription requires a known CAN Wire V1 arbitration ID")
        if not callable(handler):
            raise TypeError("handler must be callable")
        return self._runtime.subscribe(arbitration_id, handler)

    def unsubscribe(self, arbitration_id: int, handler: Subscriber) -> bool:
        """退订；身份不存在返回 False（委托运行时）。"""

        return self._runtime.unsubscribe(arbitration_id, handler)

    def get_error_count(self) -> int:
        """健康平面累计错误计数（含因容量被淘汰的记录）。"""

        return self._runtime.error_count

    def diagnostics(self) -> tuple[CanDiagnostic, ...]:
        """健康平面中类型为 CanDiagnostic 的记录快照（只读）。"""

        return tuple(item for item in self._runtime.health_records() if isinstance(item, CanDiagnostic))

    def take_external_record(self) -> CanExternalRecord | None:
        """读取 1 条不可变外部记录，而不暴露传输状态。"""

        item = self._runtime.take_external()
        return item if isinstance(item, CanExternalRecord) else None

    def external_records(self) -> tuple[CanExternalRecord, ...]:
        """返回受限只读外部投影的快照。"""

        return tuple(item for item in self._runtime.external_records() if isinstance(item, CanExternalRecord))

    def on_runtime_error(self, error: Exception) -> None:
        """运行时边界错误钩子：按链路丢失处理（失败即拒绝）。"""

        with self._runtime.lock:
            self._handle_transport_error(CanLinkLostError(str(error)))

    def on_runtime_shutdown_requested(self) -> None:
        """取消协议工作，而不关闭正在被活动轮询使用的端口。"""

        with self._runtime.lock:
            self._runtime._clear_commands_locked()
            self._pending_command = None
            self._pending_stop = None
            self._dispatching = None
            self._reset_transport_backpressure_locked()
            self._state = CanLinkState.SHUTDOWN

    def on_runtime_shutdown_timeout(self) -> None:
        self._record_diagnostic(
            CanDiagnosticCode.SHUTDOWN_TIMEOUT,
            "device runtime worker did not stop before the shutdown deadline",
        )

    def _next_dispatch_locked(self, now: float) -> _Dispatch | None:
        """挑选下一帧分发（仅锁内调用）。

        优先级：在途 STOP 的重试/超时 > 在途普通命令的重试/超时（预算耗尽时
        升级为关联 STOP）> 从命令平面弹出一帧。STOP 在途时普通流量绝不抢先。
        """

        if self._pending_stop is not None:
            return self._retry_or_timeout_locked(
                self._pending_stop,
                now,
                self._config.stop_retry_budget,
                CanDiagnosticCode.STOP_TIMEOUT,
            )
        if self._pending_command is not None:
            dispatch = self._retry_or_timeout_locked(
                self._pending_command,
                now,
                self._config.ack_retry_budget,
                CanDiagnosticCode.ACK_TIMEOUT,
            )
            if dispatch is not None or self._pending_command is not None:
                return dispatch
            # 确认重试预算耗尽：清空普通流量并发出关联 STOP（失败即拒绝升级）。
            stop_frame = self._new_stop_frame_locked()
            self._state = CanLinkState.STOPPING
            return _Dispatch(stop_frame, False)
        queued = self._runtime._pop_command_locked()
        if queued is None:
            return None
        if not isinstance(queued, CanWireFrame):
            raise RuntimeError("runtime command plane contained a non-CAN item")
        return _Dispatch(queued, False)

    def _retry_or_timeout_locked(
        self,
        pending: _PendingRequest,
        now: float,
        retry_budget: int,
        timeout_code: CanDiagnosticCode,
    ) -> _Dispatch | None:
        """在途请求的重试或超时判定（仅锁内调用）。

        期限未到返回 None（继续等待）。预算未耗尽且线上 retry_count 未达
        0xff 时返回递增重试计数的重试帧（重试保持命令关联、递增线上重试
        计数）。预算耗尽则记录超时诊断：STOP 超时进入 LINK_LOST；普通命令
        超时清空普通流量并把升级 STOP 的决定留给调用方（见
        _next_dispatch_locked）。
        """

        if now < pending.deadline:
            return None
        current_retry = pending.wire_frame.retry_count
        if current_retry is None:
            raise RuntimeError("pending command is missing retry_count")
        # 0xff 是线上字节上限：达到后无法再递增，直接按超时处理。
        if pending.retry_budget_used < retry_budget and current_retry < 0xFF:
            return _Dispatch(_with_retry_count(pending.wire_frame, current_retry + 1), True)

        command_id = pending.wire_frame.command_id
        self._record_diagnostic_locked(
            timeout_code,
            f"{pending.wire_frame.kind} acknowledgement deadline expired",
            command_id,
        )
        # 记住已发送的全部 retry_count：迟到的回显将按 LATE 拒绝、绝不确认。
        self._remember_correlation_locked(self._timed_out, self._timed_out_order, pending)
        if pending.wire_frame.kind is CanFrameKind.STOP:
            # STOP 超时：失败关闭链路，普通命令在恢复前被禁用。
            self._pending_stop = None
            self._runtime._clear_commands_locked()
            self._state = CanLinkState.LINK_LOST
        else:
            self._pending_command = None
            self._runtime._clear_commands_locked()
        return None

    def _record_dispatch_locked(self, dispatch: _Dispatch, now: float) -> None:
        """在成功分发后记账（仅锁内调用）。

        首次分发创建 :class:`_PendingRequest` 并开启确认期限；重试分发更新
        在途帧、重置期限、递增已消耗预算并把新的 retry_count 加入已发送
        集合（MCU 回显任一者即可关联）。STOP 分发使链路进入 STOPPING。
        """

        wire_frame = dispatch.wire_frame
        retry_count = wire_frame.retry_count
        if retry_count is None:
            raise RuntimeError("outbound command is missing retry_count")
        if wire_frame.kind is CanFrameKind.COMMAND:
            if dispatch.is_retry:
                pending = self._pending_command
                if pending is None:
                    raise RuntimeError("ordinary retry has no pending command")
                pending.wire_frame = wire_frame
                pending.deadline = now + self._config.ack_timeout_s
                pending.retry_budget_used += 1
                pending.sent_retry_counts.add(retry_count)
            else:
                # 首次分发：确认期限从现在起算（本地分发开始计时，不是命令完成）。
                self._pending_command = _PendingRequest(
                    wire_frame,
                    now + self._config.ack_timeout_s,
                    0,
                    {retry_count},
                )
        elif wire_frame.kind is CanFrameKind.STOP:
            # STOP 在途：链路进入 STOPPING，普通流量不得派发。
            self._state = CanLinkState.STOPPING
            if dispatch.is_retry:
                pending = self._pending_stop
                if pending is None:
                    raise RuntimeError("STOP retry has no pending STOP")
                pending.wire_frame = wire_frame
                pending.deadline = now + self._config.stop_timeout_s
                pending.retry_budget_used += 1
                pending.sent_retry_counts.add(retry_count)
            else:
                self._pending_stop = _PendingRequest(
                    wire_frame,
                    now + self._config.stop_timeout_s,
                    0,
                    {retry_count},
                )

    def _handle_received(self, frame: object) -> CanReceiveResult | None:
        """处理 1 条入站传输记录（错误帧、Wire V1 校验、关联、投影、回调）。

        错误帧绝不进入 Wire V1 解码；bus-off 位单独检查使适配器进入
        ``BUS_OFF``。有效入站帧按关联/遥测排序规则处理，接受的记录发布进
        受限外部投影；重复、迟到、无关联、畸形帧保持可观测拒绝但绝不进入
        外部队列，也不能声称完成。运行时关闭期间到达的帧不被分发或发布。
        """

        with self._runtime.lock:
            if not self._runtime_accepts_ingress_locked():
                return None
        if isinstance(frame, CanFrame) and frame.is_error_frame:
            # 错误帧是内核状态记录，不是 Wire V1 协议帧；保留可观测拒绝。
            displayed_id = (
                f"0x{frame.arbitration_id:x}" if type(frame.arbitration_id) is int else repr(frame.arbitration_id)
            )
            reason = f"CAN error frame {displayed_id} is not a Wire V1 protocol frame"
            with self._runtime.lock:
                # bus-off 位：控制器失去仲裁能力，挂起工作必须清空（失败即拒绝）。
                if type(frame.arbitration_id) is int and frame.arbitration_id & _CAN_ERR_BUSOFF:
                    self._handle_transport_error(CanBusOffError("SocketCAN reported CAN bus-off"))
                self._record_diagnostic_locked(CanDiagnosticCode.INVALID_FRAME, reason)
                record = self._make_external_record_locked(
                    CanReceiveStatus.INVALID_FRAME,
                    frame=frame,
                    reason=reason,
                    frame_valid=False,
                    exposure_allowed=False,
                )
            return CanReceiveResult(CanReceiveStatus.INVALID_FRAME, reason=reason, external_record=record)

        try:
            # Wire V1 完整校验：ID、DLC、版本、保留字节、枚举与跨字段语义。
            wire_frame = decode_can_frame(frame)
        except CanFrameValidationError as exc:
            with self._runtime.lock:
                if not self._runtime_accepts_ingress_locked():
                    return None
                self._record_diagnostic_locked(CanDiagnosticCode.INVALID_FRAME, str(exc))
                record = self._make_external_record_locked(
                    CanReceiveStatus.INVALID_FRAME,
                    frame=frame if isinstance(frame, CanFrame) else None,
                    reason=str(exc),
                    frame_valid=False,
                    exposure_allowed=False,
                )
            return CanReceiveResult(CanReceiveStatus.INVALID_FRAME, reason=str(exc), external_record=record)
        if wire_frame.kind not in _INBOUND_KINDS:
            # 入站方向仅允许 ack/stop_ack/遥测；主机到 MCU 的帧回环即拒绝。
            reason = "transport receive path only accepts ack, stop_ack, and telemetry frames"
            with self._runtime.lock:
                if not self._runtime_accepts_ingress_locked():
                    return None
                self._record_diagnostic_locked(CanDiagnosticCode.INVALID_FRAME, reason, wire_frame.command_id)
                record = self._make_external_record_locked(
                    CanReceiveStatus.INVALID_FRAME,
                    frame=wire_frame.frame,
                    wire_frame=wire_frame,
                    reason=reason,
                    frame_valid=True,
                    exposure_allowed=False,
                )
            return CanReceiveResult(
                CanReceiveStatus.INVALID_FRAME,
                wire_frame=wire_frame,
                reason=reason,
                external_record=record,
            )

        with self._runtime.lock:
            if not self._runtime_accepts_ingress_locked():
                return None
            # 构造入口信封：非有限/倒退时钟观测在信封阶段失败即拒绝。
            envelope = self._make_envelope_locked(wire_frame)
            if envelope is None:
                observation = self._last_observation
                reason = "invalid ingress timestamp observation"
                if observation is not None and observation.reason:
                    reason = observation.reason
                record = self._make_external_record_locked(
                    CanReceiveStatus.INVALID_FRAME,
                    frame=wire_frame.frame,
                    wire_frame=wire_frame,
                    reason=reason,
                    frame_valid=False,
                    exposure_allowed=False,
                )
                return CanReceiveResult(
                    CanReceiveStatus.INVALID_FRAME,
                    wire_frame=wire_frame,
                    reason=reason,
                    external_record=record,
                )
            # 关联判定：ACK/STOP_ACK 匹配在途请求；遥测按半区间序号排序。
            status, confirmed = self._correlate_received_locked(wire_frame)
            envelope = replace(envelope, health=self._state)
            if status is not CanReceiveStatus.ACCEPTED:
                # 重复/迟到/无关联：可观测拒绝，刻意排除在外部投影之外。
                record = self._make_external_record_locked(
                    status,
                    frame=wire_frame.frame,
                    wire_frame=wire_frame,
                    envelope=envelope,
                    reason="duplicate or late ingress was not exposed as a new event",
                    frame_valid=True,
                    exposure_allowed=False,
                )
                return CanReceiveResult(
                    status,
                    wire_frame=wire_frame,
                    envelope=envelope,
                    confirmed=False,
                    external_record=record,
                )
            if wire_frame.kind is CanFrameKind.TELEMETRY:
                # 遥测走受限遥测平面（容量固定、最旧淘汰、丢弃计数）。
                self._runtime.publish_telemetry(wire_frame)
                dispatch_frame = self._runtime.take_telemetry()
            else:
                dispatch_frame = wire_frame

        if not isinstance(dispatch_frame, CanWireFrame):
            raise RuntimeError("telemetry plane lost an accepted CAN frame")
        with self._runtime.lock:
            if not self._runtime_accepts_ingress_locked():
                return None
        # 回调在锁外派发（不可变订阅者快照）；异常被隔离并计数。
        callback_errors = self._runtime.dispatch_callbacks(dispatch_frame)
        if callback_errors:
            with self._runtime.lock:
                if not self._runtime_accepts_ingress_locked():
                    return None
                for _ in range(callback_errors):
                    self._record_diagnostic_locked(
                        CanDiagnosticCode.SUBSCRIBER_ERROR,
                        "subscriber callback raised an exception",
                        wire_frame.command_id,
                    )
                record = self._make_external_record_locked(
                    CanReceiveStatus.ACCEPTED,
                    frame=wire_frame.frame,
                    wire_frame=wire_frame,
                    envelope=envelope,
                    frame_valid=True,
                    exposure_allowed=True,
                    confirmed=confirmed,
                    callback_errors=callback_errors,
                )
        else:
            with self._runtime.lock:
                if not self._runtime_accepts_ingress_locked():
                    return None
                record = self._make_external_record_locked(
                    CanReceiveStatus.ACCEPTED,
                    frame=wire_frame.frame,
                    wire_frame=wire_frame,
                    envelope=envelope,
                    frame_valid=True,
                    exposure_allowed=True,
                    confirmed=confirmed,
                    callback_errors=callback_errors,
                )
        return CanReceiveResult(
            CanReceiveStatus.ACCEPTED,
            wire_frame=wire_frame,
            envelope=envelope,
            confirmed=confirmed,
            callback_errors=callback_errors,
            external_record=record,
        )

    def _make_envelope_locked(self, wire_frame: CanWireFrame) -> CanTransportEnvelope | None:
        """构造入口信封（仅锁内调用）。

        时钟观测失败（非有限/倒退）时返回 None，帧按失败即拒绝处理；
        信封携带入口序号、双时钟观测、内核元数据与证据引用。
        """

        observation = self._observe_ingress_locked(wire_frame.frame)
        self._last_observation = observation
        if not observation.valid:
            return None
        self._last_observation = None
        evidence_ref = self._evidence_ref(observation.sequence)
        return CanTransportEnvelope(
            source=self._config.source,
            interface=self._config.interface,
            sequence=observation.sequence,
            monotonic_ts=observation.monotonic_ts,
            wall_ts=observation.wall_ts,
            health=self._state,
            wire_frame=wire_frame,
            evidence_refs=(evidence_ref,),
            kernel_timestamp_ns=observation.kernel_timestamp_ns,
            kernel_drop_count=observation.kernel_drop_count,
            timestamp_source=observation.timestamp_source,
        )

    def _observe_ingress_locked(self, frame: CanFrame | None) -> _IngressObservation:
        """对一次入口做序号与时钟观测（仅锁内调用）。

        入口序号单调递增、绝不因恢复重置；帧提供的主机观测优先于注入时钟；
        非有限或倒退的单调/墙上观测是失败即拒绝（同时触发 CLOCK_ROLLBACK
        诊断与链路丢失），不会被静默归一化。时间戳来源标记 kernel/host/
        kernel+host/adapter。
        """

        sequence = self._ingress_sequence
        self._ingress_sequence += 1
        raw_kernel_timestamp_ns = None if frame is None else frame.kernel_timestamp_ns
        raw_kernel_drop_count = None if frame is None else frame.kernel_drop_count
        # 畸形内核元数据降级为 None，而不是拒绝整个帧。
        kernel_timestamp_ns = (
            raw_kernel_timestamp_ns
            if raw_kernel_timestamp_ns is None
            or (type(raw_kernel_timestamp_ns) is int and raw_kernel_timestamp_ns >= 0)
            else None
        )
        kernel_drop_count = (
            raw_kernel_drop_count
            if raw_kernel_drop_count is None
            or (type(raw_kernel_drop_count) is int and 0 <= raw_kernel_drop_count <= 0xFFFFFFFF)
            else None
        )

        def invalid(reason: str, *, clock_failure: bool = False) -> _IngressObservation:
            # clock_failure 同时把链路打失败（时钟故障即失败关闭）。
            if clock_failure:
                self._fail_clock_locked(reason)
            return _IngressObservation(
                sequence=sequence,
                monotonic_ts=None,
                wall_ts=None,
                kernel_timestamp_ns=kernel_timestamp_ns,
                kernel_drop_count=kernel_drop_count,
                timestamp_source="none",
                valid=False,
                reason=reason,
            )

        provided_monotonic = None if frame is None else frame.observed_monotonic_ts
        valid_provided_monotonic = (
            provided_monotonic is not None
            and not isinstance(provided_monotonic, bool)
            and isinstance(provided_monotonic, int | float)
            and math.isfinite(provided_monotonic)
        )

        try:
            monotonic_value = provided_monotonic if valid_provided_monotonic else self._clock.monotonic()
        except Exception as exc:  # noqa: BLE001 - clock faults fail closed at ingress.
            return invalid(f"monotonic clock failed: {exc}", clock_failure=True)
        if (
            isinstance(monotonic_value, bool)
            or not isinstance(monotonic_value, int | float)
            or not math.isfinite(monotonic_value)
        ):
            return invalid("monotonic observation is not finite", clock_failure=True)
        monotonic_ts = float(monotonic_value)
        # 单调观测不得倒退（时钟回拨是可观测的失败即拒绝入口错误）。
        if self._last_monotonic_ts is not None and monotonic_ts < self._last_monotonic_ts:
            return invalid("monotonic observation moved backwards", clock_failure=True)

        provided_wall = None if frame is None else frame.observed_wall_ts
        valid_provided_wall = (
            provided_wall is not None
            and not isinstance(provided_wall, bool)
            and isinstance(provided_wall, int | float)
            and math.isfinite(provided_wall)
        )

        try:
            wall_value = provided_wall if valid_provided_wall else self._wall_clock()
        except Exception as exc:  # noqa: BLE001 - clock faults fail closed at ingress.
            return invalid(f"wall clock failed: {exc}", clock_failure=True)
        if isinstance(wall_value, bool) or not isinstance(wall_value, int | float) or not math.isfinite(wall_value):
            return invalid("wall-clock observation is not finite", clock_failure=True)
        wall_ts = float(wall_value)
        # 墙上观测同样不得倒退（NTP 回拨等）。
        if self._last_wall_ts is not None and wall_ts < self._last_wall_ts:
            return invalid("wall-clock observation moved backwards", clock_failure=True)
        self._last_monotonic_ts = monotonic_ts
        self._last_wall_ts = wall_ts
        has_host_timestamp = valid_provided_monotonic or valid_provided_wall
        # 时间戳来源：内核与主机并存时标 kernel+host，否则按可用性递减。
        timestamp_source = (
            "kernel+host"
            if kernel_timestamp_ns is not None and has_host_timestamp
            else "kernel"
            if kernel_timestamp_ns is not None
            else "host"
            if has_host_timestamp
            else "adapter"
        )
        return _IngressObservation(
            sequence=sequence,
            monotonic_ts=monotonic_ts,
            wall_ts=wall_ts,
            kernel_timestamp_ns=kernel_timestamp_ns,
            kernel_drop_count=kernel_drop_count,
            timestamp_source=timestamp_source,
            valid=True,
        )

    def _make_external_record_locked(
        self,
        status: CanReceiveStatus,
        *,
        frame: CanFrame | None = None,
        wire_frame: CanWireFrame | None = None,
        envelope: CanTransportEnvelope | None = None,
        reason: str | None = None,
        frame_valid: bool,
        exposure_allowed: bool,
        confirmed: bool = False,
        callback_errors: int = 0,
    ) -> CanExternalRecord:
        """构造外部投影记录（仅锁内调用）。

        优先复用信封的观测与证据引用；无信封时复用最近一次观测。
        ``exposure_allowed`` 且运行时仍接受入口时发布进受限外部队列
        （最旧淘汰），消费者落后时记录 EXTERNAL_BACKPRESSURE。
        """

        source_frame = wire_frame.frame if wire_frame is not None else frame
        if envelope is not None:
            sequence = envelope.sequence
            monotonic_ts = envelope.monotonic_ts
            wall_ts = envelope.wall_ts
            kernel_timestamp_ns = envelope.kernel_timestamp_ns
            kernel_drop_count = envelope.kernel_drop_count
            timestamp_source = envelope.timestamp_source
            health = envelope.health
            evidence_refs = envelope.evidence_refs
        else:
            # 无信封（早期拒绝路径）：复用最近一次观测，避免重复生成。
            observation = self._last_observation or self._observe_ingress_locked(source_frame)
            self._last_observation = None
            sequence = observation.sequence
            monotonic_ts = observation.monotonic_ts
            wall_ts = observation.wall_ts
            kernel_timestamp_ns = observation.kernel_timestamp_ns
            kernel_drop_count = observation.kernel_drop_count
            timestamp_source = observation.timestamp_source
            health = self._state
            evidence_refs = (self._evidence_ref(sequence),)

        kind = None if wire_frame is None else wire_frame.kind
        # 帧元数据只做安全投影：矛盾 raw ID 被清空，不重现校验故障。
        frame_metadata = _safe_external_frame_metadata(source_frame, frame_valid=frame_valid)
        record = CanExternalRecord(
            status=status,
            source=self._config.source,
            interface=self._config.interface,
            ingress_sequence=sequence,
            health=health,
            frame_valid=frame_valid,
            exposure_allowed=exposure_allowed,
            event_type=_external_event_type(kind),
            frame_kind=kind,
            arbitration_id=frame_metadata["arbitration_id"],
            raw_can_id=frame_metadata["raw_can_id"],
            dlc=frame_metadata["dlc"],
            data=frame_metadata["data"],
            is_extended_id=frame_metadata["is_extended_id"],
            is_remote_frame=frame_metadata["is_remote_frame"],
            is_error_frame=frame_metadata["is_error_frame"],
            monotonic_ts=monotonic_ts,
            wall_ts=wall_ts,
            kernel_timestamp_ns=kernel_timestamp_ns,
            kernel_drop_count=kernel_drop_count,
            timestamp_source=timestamp_source,
            reason=reason,
            callback_errors=callback_errors,
            confirmed=confirmed,
            command_id=None if wire_frame is None else wire_frame.command_id,
            sequence_no=None if wire_frame is None else wire_frame.sequence_no,
            opcode=None if wire_frame is None else wire_frame.opcode,
            retry_count=None if wire_frame is None else wire_frame.retry_count,
            result_code=None if wire_frame is None else wire_frame.result_code,
            fault_code=None if wire_frame is None else wire_frame.fault_code,
            device_mode=None if wire_frame is None else wire_frame.device_mode,
            evidence_refs=tuple(evidence_refs),
        )
        if exposure_allowed and self._runtime_accepts_ingress_locked():
            dropped_before = self._runtime.external_drop_count
            self._runtime.publish_external(record)
            if self._runtime.external_drop_count > dropped_before:
                self._record_diagnostic_locked(
                    CanDiagnosticCode.EXTERNAL_BACKPRESSURE,
                    "external consumer was slower than the bounded projection plane",
                )
        return record

    def _evidence_ref(self, sequence: int) -> str:
        """为入口序号生成不可变证据引用（can-ingress:// URI，URL 转义身份）。"""

        return (
            f"can-ingress://{quote(self._config.source, safe='')}/{quote(self._config.interface, safe='')}/{sequence}"
        )

    def _handle_transport_frame_error(self, error: CanTransportFrameError) -> CanReceiveResult | None:
        """把传输记录格式错误转换为可观测拒绝（链路可能仍可用）。"""

        reason = str(error)
        with self._runtime.lock:
            if not self._runtime_accepts_ingress_locked():
                return None
            self._record_diagnostic_locked(CanDiagnosticCode.INVALID_FRAME, reason)
            record = self._make_external_record_locked(
                CanReceiveStatus.INVALID_FRAME,
                reason=reason,
                frame_valid=False,
                exposure_allowed=False,
            )
        return CanReceiveResult(CanReceiveStatus.INVALID_FRAME, reason=reason, external_record=record)

    def _runtime_accepts_ingress_locked(self) -> bool:
        """运行时是否仍接受入口（仅锁内调用）：ACTIVE 且关闭未请求。"""

        return self._runtime.state is DeviceRuntimeState.ACTIVE and not self._runtime._shutdown_requested

    def _fail_clock_locked(self, detail: str) -> None:
        """时钟故障失败关闭链路并记录 CLOCK_ROLLBACK 诊断（仅锁内调用）。"""

        self._handle_transport_error(CanLinkLostError(detail))
        self._record_diagnostic_locked(CanDiagnosticCode.CLOCK_ROLLBACK, detail)

    def _correlate_received_locked(self, wire_frame: CanWireFrame) -> tuple[CanReceiveStatus, bool]:
        """入站关联判定（仅锁内调用）。

        遥测按 Wire V1 半区间序号排序：delta=0 重复、delta>=2^31 视为过期
        （32 位回绕语义），接受的快照推进基线；故障遥测额外失败关闭链路。
        ACK/STOP_ACK 匹配在途请求（键 = 响应类型+command_id+opcode，且
        retry_count 在已发送集合内）：STOP_ACK 接受进入 SAFE_STOPPED、拒绝
        进入 LINK_LOST；普通 ACK 拒绝清空流量并进入 LINK_LOST。已完成的
        retry_count 重复为 DUPLICATE，超时后的为 LATE，其余 UNCORRELATED。
        """

        if wire_frame.kind is CanFrameKind.TELEMETRY:
            sequence_no = wire_frame.sequence_no
            if sequence_no is None:
                raise RuntimeError("telemetry is missing sequence_no")
            if self._last_telemetry_sequence is not None:
                # 半区间差：无符号 32 位差 < 2^31 视为新快照，否则过期/歧义。
                delta = (sequence_no - self._last_telemetry_sequence) & 0xFFFFFFFF
                if delta == 0:
                    self._record_diagnostic_locked(
                        CanDiagnosticCode.DUPLICATE_TELEMETRY,
                        "duplicate telemetry ignored",
                    )
                    return CanReceiveStatus.DUPLICATE, False
                if delta >= 0x80000000:
                    self._record_diagnostic_locked(
                        CanDiagnosticCode.STALE_TELEMETRY,
                        "stale or ambiguous telemetry ignored",
                    )
                    return CanReceiveStatus.LATE, False
            self._last_telemetry_sequence = sequence_no
            # 故障遥测（link_lost/watchdog_expired）在显式恢复前禁用普通流量。
            if wire_frame.fault_code != _WireFault.NONE:
                self._handle_transport_error(
                    CanLinkLostError(f"MCU fault telemetry reported fault code {wire_frame.fault_code}")
                )
            return CanReceiveStatus.ACCEPTED, False
        key = _correlation_key(wire_frame)
        retry_count = wire_frame.retry_count
        if retry_count is None:
            raise RuntimeError("acknowledgement is missing retry_count")
        pending = self._pending_stop if wire_frame.kind is CanFrameKind.STOP_ACK else self._pending_command
        if (
            pending is not None
            and key == _response_key(pending.wire_frame)
            and retry_count in pending.sent_retry_counts
        ):
            # 匹配在途请求：进入有界完成窗口，供后续重复/迟到判定。
            self._remember_correlation_locked(self._completed, self._completed_order, pending)
            if wire_frame.kind is CanFrameKind.STOP_ACK:
                self._pending_stop = None
                self._runtime._clear_commands_locked()
                if wire_frame.result_code == _WireResult.ACCEPTED:
                    self._state = CanLinkState.SAFE_STOPPED
                else:
                    # STOP 被拒绝：失败关闭链路，普通命令在恢复前被禁用。
                    self._state = CanLinkState.LINK_LOST
                    self._record_diagnostic_locked(
                        CanDiagnosticCode.STOP_REJECTED,
                        "MCU returned a rejected STOP acknowledgement",
                        wire_frame.command_id,
                    )
            else:
                self._pending_command = None
                if wire_frame.result_code != _WireResult.ACCEPTED:
                    # 普通 ACK 被拒绝（duplicate_frame/malformed_frame）：失败关闭。
                    self._runtime._clear_commands_locked()
                    self._state = CanLinkState.LINK_LOST
                    self._record_diagnostic_locked(
                        CanDiagnosticCode.COMMAND_REJECTED,
                        "MCU returned a rejected ordinary acknowledgement",
                        wire_frame.command_id,
                    )
                    return CanReceiveStatus.ACCEPTED, False
            return CanReceiveStatus.ACCEPTED, wire_frame.result_code == _WireResult.ACCEPTED

        if retry_count in self._completed.get(key, ()):
            # 重复确认：原始关联结果已返回，重复帧绝不更改状态或声称完成。
            self._record_diagnostic_locked(
                CanDiagnosticCode.DUPLICATE_ACK,
                "duplicate acknowledgement ignored",
                wire_frame.command_id,
            )
            return CanReceiveStatus.DUPLICATE, False
        if retry_count in self._timed_out.get(key, ()):
            # 迟到确认：绝不变更较新的动作或声称完成（robot-bsp-can-v0.1.md）。
            self._record_diagnostic_locked(
                CanDiagnosticCode.LATE_ACK,
                "late acknowledgement did not change transport state",
                wire_frame.command_id,
            )
            return CanReceiveStatus.LATE, False
        self._record_diagnostic_locked(
            CanDiagnosticCode.UNCORRELATED_ACK,
            "acknowledgement did not match a dispatched attempt",
            wire_frame.command_id,
        )
        return CanReceiveStatus.UNCORRELATED, False

    def _handle_transport_backpressure(self, error: CanTransportBackpressureError) -> None:
        """内核传输背压处理（仅锁内调用）。

        相同分发键的连续背压计入预算；预算内把该帧放回队列头等待重试，
        超额即失败关闭链路（LINK_LOST），绝不无限重试。
        """

        with self._runtime.lock:
            dispatch = self._dispatching
            self._dispatching = None
            self._record_diagnostic_locked(CanDiagnosticCode.TRANSPORT_BACKPRESSURE, str(error))
            if dispatch is None:
                return
            key = (
                dispatch.wire_frame.kind,
                dispatch.wire_frame.command_id,
                dispatch.wire_frame.retry_count,
            )
            if key == self._transport_backpressure_key:
                self._transport_backpressure_attempts += 1
            else:
                self._transport_backpressure_key = key
                self._transport_backpressure_attempts = 1
            # 超过配置预算：内核持续满视为链路不可用（失败即拒绝）。
            if self._transport_backpressure_attempts > self._config.transport_backpressure_budget:
                self._handle_transport_error(
                    CanLinkLostError(
                        "SocketCAN send remained backpressured beyond the bounded "
                        f"retry budget ({self._config.transport_backpressure_budget})"
                    )
                )
                return
            # 无在途请求时把该帧放回队列头；有在途请求时由重试逻辑接管。
            if self._pending_command is None and self._pending_stop is None:
                if dispatch.wire_frame.kind is CanFrameKind.STOP:
                    self._runtime._clear_commands_locked()
                self._runtime._prepend_command_locked(dispatch.wire_frame)

    def _handle_transport_error(self, error: CanTransportError) -> None:
        """传输级错误统一处理（仅锁内调用）。

        清空在途请求与命令平面、把已发送的 retry_count 记入超时窗口（迟到的
        回显按 LATE 拒绝）、重置背压统计。bus-off 进入 BUS_OFF，其余进入
        LINK_LOST；两者都在显式恢复成功前禁用普通命令。SHUTDOWN 状态下
        不再改写链路标签。
        """

        with self._runtime.lock:
            if self._pending_command is not None:
                self._remember_correlation_locked(
                    self._timed_out,
                    self._timed_out_order,
                    self._pending_command,
                )
            if self._pending_stop is not None:
                self._remember_correlation_locked(
                    self._timed_out,
                    self._timed_out_order,
                    self._pending_stop,
                )
            self._pending_command = None
            self._pending_stop = None
            self._dispatching = None
            self._reset_transport_backpressure_locked()
            self._runtime._clear_commands_locked()
            if isinstance(error, CanBusOffError):
                code = CanDiagnosticCode.BUS_OFF
                next_state = CanLinkState.BUS_OFF
            else:
                code = CanDiagnosticCode.LINK_LOST
                next_state = CanLinkState.LINK_LOST
            if self._state is not CanLinkState.SHUTDOWN:
                self._state = next_state
            self._record_diagnostic_locked(code, str(error))

    def _preempt_for_stop_locked(self, detail: str) -> None:
        """STOP 抢占排队与在途普通流量（仅锁内调用）。

        清空命令平面与传输背压统计、链路进入 STOPPING；在途/正在分发的
        普通命令被记入超时关联窗口（迟到 ACK 绝不确认），并记录
        STOP_PREEMPTED 诊断。
        """

        preempted = self._runtime.command_depth
        self._runtime._clear_commands_locked()
        self._reset_transport_backpressure_locked()
        self._state = CanLinkState.STOPPING
        if self._pending_command is not None:
            sent_retry_counts = set(self._pending_command.sent_retry_counts)
            if self._dispatching is not None and self._dispatching.wire_frame.kind is CanFrameKind.COMMAND:
                retry_count = self._dispatching.wire_frame.retry_count
                if retry_count is not None:
                    sent_retry_counts.add(retry_count)
            self._remember_correlation_locked(
                self._timed_out,
                self._timed_out_order,
                _PendingRequest(
                    self._pending_command.wire_frame,
                    self._pending_command.deadline,
                    self._pending_command.retry_budget_used,
                    sent_retry_counts,
                ),
            )
            self._pending_command = None
            preempted += 1
        elif self._dispatching is not None and self._dispatching.wire_frame.kind is CanFrameKind.COMMAND:
            # 命令已在锁外 send 中：连同其 retry_count 记入超时窗口。
            retry_count = self._dispatching.wire_frame.retry_count
            if retry_count is None:
                raise RuntimeError("dispatching command is missing retry_count")
            self._remember_correlation_locked(
                self._timed_out,
                self._timed_out_order,
                _PendingRequest(self._dispatching.wire_frame, 0.0, 0, {retry_count}),
            )
            preempted += 1
        if self._dispatching is not None and self._dispatching.wire_frame.kind is CanFrameKind.COMMAND:
            self._dispatching = None
        if preempted:
            self._record_diagnostic_locked(CanDiagnosticCode.STOP_PREEMPTED, detail)

    def _new_stop_frame_locked(self) -> CanWireFrame:
        """生成一条关联 STOP 帧（仅锁内调用）。

        command_id 从 ``initial_stop_command_id`` 递增，0xffff 后回绕到
        0x8000（STOP 分区内的有界回绕）；跳过仍被关联窗口保留的 ID，
        遍历满整个窗口仍未找到可用 ID 即抛出 RuntimeError。
        """

        for _ in range(self._config.correlation_capacity + 1):
            command_id = self._next_stop_command_id
            # STOP 分区回绕：0xffff 的下一个是 0x8000。
            self._next_stop_command_id = 0x8000 if command_id == 0xFFFF else command_id + 1
            if not self._correlation_id_in_use_locked(CanFrameKind.STOP, command_id):
                # 载荷布局：0=version，1..2=command_id（大端），3=opcode stop，
                # 4=retry_count 0，5..7=保留零（mcu-wire-v1.md Command 与 STOP）。
                data = bytes(
                    [
                        MCU_WIRE_VERSION_V1,
                        command_id >> 8,
                        command_id & 0xFF,
                        _WireOpcode.STOP,
                        0,
                        0,
                        0,
                        0,
                    ]
                )
                return decode_can_frame(CanFrame(MCU_CAN_ID_STOP, data))
        raise RuntimeError("bounded STOP correlation window exhausted")

    def _correlation_id_in_use_locked(self, kind: CanFrameKind, command_id: int | None) -> bool:
        """命令 ID 是否被排队/在途/关联窗口占用（仅锁内调用）。

        普通命令与 STOP 使用各自分区，检查按请求类型归一：候选 = 命令平面
        快照 + 正在分发帧 + 在途请求；再检查 completed/timed_out 窗口中的
        响应键。任何冲突都拒绝复用（robot-bsp-can-v0.1.md 故障行为）。
        """

        if command_id is None:
            return False
        request_kind = CanFrameKind.STOP if kind in {CanFrameKind.STOP, CanFrameKind.STOP_ACK} else CanFrameKind.COMMAND
        candidates = [item for item in self._runtime._command_snapshot_locked() if isinstance(item, CanWireFrame)]
        if self._dispatching is not None:
            candidates.append(self._dispatching.wire_frame)
        if self._pending_command is not None:
            candidates.append(self._pending_command.wire_frame)
        if self._pending_stop is not None:
            candidates.append(self._pending_stop.wire_frame)
        if any(candidate.kind is request_kind and candidate.command_id == command_id for candidate in candidates):
            return True
        response_kind = CanFrameKind.STOP_ACK if request_kind is CanFrameKind.STOP else CanFrameKind.ACK
        return any(key[0] is response_kind and key[1] == command_id for key in (*self._completed, *self._timed_out))

    def _remember_correlation_locked(
        self,
        target: dict[CorrelationKey, frozenset[int]],
        order: deque[CorrelationKey],
        pending: _PendingRequest,
    ) -> None:
        """把已发送 retry_count 集合记入有界关联窗口（仅锁内调用）。

        同一键刷新为最新集合（重复入窗时移到最尾）；窗口容量固定，最旧
        键被淘汰——此后该 ID 可被复用，迟到的确认成为 UNCORRELATED。
        """

        key = _response_key(pending.wire_frame)
        if key in target:
            try:
                order.remove(key)
            except ValueError:
                pass
        target[key] = frozenset(pending.sent_retry_counts)
        order.append(key)
        while len(order) > self._config.correlation_capacity:
            target.pop(order.popleft(), None)

    def _record_diagnostic_locked(
        self,
        code: CanDiagnosticCode,
        detail: str,
        command_id: int | None = None,
    ) -> None:
        """写入一条诊断到健康平面（仅锁内调用，计入错误计数）。"""

        self._runtime._record_health_locked(CanDiagnostic(code, self._safe_monotonic_locked(), detail, command_id))

    def _reset_transport_backpressure_locked(self) -> None:
        """重置内核传输背压统计（仅锁内调用）。"""

        self._transport_backpressure_key = None
        self._transport_backpressure_attempts = 0

    def _safe_monotonic_locked(self) -> float:
        """即使注入的时钟本身发生故障，也为诊断记录时间戳。"""

        try:
            value = self._clock.monotonic()
        except Exception:  # noqa: BLE001 - 时钟故障期间诊断必须保持受限。
            return time.monotonic()
        if isinstance(value, bool) or not isinstance(value, int | float) or not math.isfinite(value):
            return time.monotonic()
        return float(value)

    def _record_diagnostic(
        self,
        code: CanDiagnosticCode,
        detail: str,
        command_id: int | None = None,
    ) -> None:
        """线程安全版诊断记录（非锁内调用入口）。"""

        with self._runtime.lock:
            self._record_diagnostic_locked(code, detail, command_id)


def _external_event_type(kind: CanFrameKind | None) -> str | None:
    """把帧类型映射为外部事件类型：遥测 -> telemetry，确认 -> action_result。"""

    if kind is CanFrameKind.TELEMETRY:
        return "telemetry"
    if kind in {CanFrameKind.ACK, CanFrameKind.STOP_ACK}:
        return "action_result"
    return None


def _safe_external_frame_metadata(
    frame: CanFrame | None,
    *,
    frame_valid: bool,
) -> dict[str, object | None]:
    """投影格式不合法的入口元数据，而不复现该故障。"""

    if not isinstance(frame, CanFrame):
        return {
            "arbitration_id": None,
            "raw_can_id": None,
            "dlc": None,
            "data": None,
            "is_extended_id": None,
            "is_remote_frame": None,
            "is_error_frame": None,
        }

    # 非整数/负的仲裁标识符投影为 None，而不是抛出异常。
    arbitration_id = frame.arbitration_id if type(frame.arbitration_id) is int and frame.arbitration_id >= 0 else None
    flags: dict[str, bool | None] = {
        name: getattr(frame, name) if type(getattr(frame, name)) is bool else None
        for name in ("is_extended_id", "is_remote_frame", "is_error_frame")
    }
    raw_can_id = frame.raw_can_id if type(frame.raw_can_id) is int and 0 <= frame.raw_can_id <= 0xFFFFFFFF else None
    if raw_can_id is not None and arbitration_id is not None and all(value is not None for value in flags.values()):
        # raw_can_id 必须与仲裁标识符加 EFF/RTR/ERR 高位标志一致，否则清空。
        expected = arbitration_id
        if flags["is_extended_id"]:
            expected |= _CAN_EFF_FLAG
        if flags["is_remote_frame"]:
            expected |= _CAN_RTR_FLAG
        if flags["is_error_frame"]:
            expected |= _CAN_ERR_FLAG
        if raw_can_id != expected:
            raw_can_id = None
    else:
        raw_can_id = None

    # DLC 优先取帧字段，其次用数据长度推断；越界时清空。
    if type(frame.dlc) is int and 0 <= frame.dlc <= 8:
        dlc: int | None = frame.dlc
    elif isinstance(frame.data, bytes) and len(frame.data) <= 8:
        dlc = len(frame.data)
    else:
        dlc = None
    # 数据只在校验通过时投影（frame_valid=False 时绝不携带原始字节）。
    data = frame.data if frame_valid and isinstance(frame.data, bytes) else None
    return {
        "arbitration_id": arbitration_id,
        "raw_can_id": raw_can_id,
        "dlc": dlc,
        "data": data,
        **flags,
    }


def _with_retry_count(wire_frame: CanWireFrame, retry_count: int) -> CanWireFrame:
    """构造只把载荷字节 4 换成新重试计数的帧（重试保持命令关联）。"""

    data = bytearray(wire_frame.frame.data)
    data[4] = retry_count
    return decode_can_frame(
        CanFrame(
            wire_frame.frame.arbitration_id,
            bytes(data),
            is_extended_id=wire_frame.frame.is_extended_id,
            is_remote_frame=wire_frame.frame.is_remote_frame,
            is_error_frame=wire_frame.frame.is_error_frame,
            dlc=wire_frame.frame.dlc,
        )
    )


def _correlation_key(wire_frame: CanWireFrame) -> CorrelationKey:
    """入站确认的关联键：(帧类型, command_id, opcode)。"""

    if wire_frame.command_id is None or wire_frame.opcode is None:
        raise RuntimeError("correlated frame is missing command fields")
    return wire_frame.kind, wire_frame.command_id, wire_frame.opcode


def _response_key(wire_frame: CanWireFrame) -> CorrelationKey:
    """出站请求对应的响应键：COMMAND -> ACK，STOP -> STOP_ACK。"""

    if wire_frame.command_id is None or wire_frame.opcode is None:
        raise RuntimeError("correlated frame is missing command fields")
    response_kind = CanFrameKind.ACK if wire_frame.kind is CanFrameKind.COMMAND else CanFrameKind.STOP_ACK
    return response_kind, wire_frame.command_id, wire_frame.opcode
