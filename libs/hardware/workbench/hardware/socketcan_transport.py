"""基于标准库的 SocketCAN 传输层，服务于受限 CAN 运行时。

本模块只持有 1 个 AF_CAN/CAN_RAW 文件描述符，不包含 worker、队列或
生命周期状态机。``DeviceRuntime`` 仍是这些关注点的所有者；
:class:`SocketCANTransport` 只负责把 Linux ``struct can_frame`` 边界
翻译成不可变的 :class:`CanFrame` 值。

入口契约（host-can-transport-v1.md「SocketCAN 入口契约」），全部检查
都在 :func:`decode_can_frame` 之前执行：

1. 构造绝不打开 socket 或任何硬件设备；``open()`` 恰好打开 1 个
   ``AF_CAN``/``SOCK_RAW``/``CAN_RAW`` socket 并绑定到配置的接口，不创建
   适配器本地 worker、队列、重试循环或第二个生命周期状态机。
2. 安装类型化的 ``CAN_RAW_FILTER``：过滤器掩码包含 standard/extended/RTR
   标志位，使帧类型变体不可能意外匹配；可选的 ``CAN_RAW_ERR_FILTER``
   与协议帧过滤器分离。
3. 启用有界接收缓冲、``SO_RXQ_OVFL`` 与（默认）``SO_TIMESTAMPNS``；
   使用 ``poll()`` 与非阻塞 ``recvmsg()``，每次调用一条记录。
4. ``MSG_TRUNC``/``MSG_CTRUNC``、畸形辅助数据、短记录、CAN-FD 尺寸记录、
   无效 DLC 与矛盾的 raw-ID 标志都成为可观测的帧拒绝（失败即拒绝）。
5. 在不可变 :class:`CanFrame` 中保留 standard/extended/RTR/error 标志；
   错误帧绝不进入 Wire V1 解码，bus-off 错误由上层适配器进入 ``BUS_OFF``。

构造与发送成功都绝不意味着 MCU 接受了命令；CAN 总线重启仍是 CAN 核心/
网络管理员的运维操作，可选的恢复探针只负责告知适配器该操作已完成。
"""

from __future__ import annotations

import errno
import math
import select
import socket
import struct
import threading
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from itertools import islice
from typing import Any

from .can_driver_safe import (
    MCU_CAN_ID_ACK,
    MCU_CAN_ID_STOP_ACK,
    MCU_CAN_ID_TELEMETRY,
    CanFrame,
    CanLinkLostError,
    CanTransportBackpressureError,
    CanTransportError,
    CanTransportFrameError,
)

# Linux struct can_frame.can_id 的高位标志（include/linux/can.h）：
# EFF = extended 29 位 ID；RTR = 远程传输请求；ERR = 错误帧。
# 三者互斥组合由 pack/unpack 校验，解码后从仲裁标识符中剥离。
CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
# 标准 11 位 ID 掩码；extended 29 位 ID 掩码（去掉 3 个标志位）。
CAN_SFF_MASK = 0x000007FF
CAN_EFF_MASK = 0x1FFFFFFF
# 错误帧掩码：错误帧 ID 只保留低 29 位错误状态位（CAN_ERR_* 枚举）。
CAN_ERR_MASK = 0x1FFFFFFF
# 内核错误掩码中的 bus-off 位（CAN_ERR_BUSOFF = 0x40）：控制器已进入
# bus-off 状态，适配器据此进入 BUS_OFF 并清空挂起工作。
CAN_ERR_BUSOFF = 0x00000040
# Classic CAN 最大 DLC（0..8）；CAN-FD 尺寸记录在 unpack 中被拒绝。
CAN_MAX_DLC = 8
# Linux CAN_RAW_FILTER 的最大条目数（CAN_RAW_FILTER_MAX = 512）。
CAN_RAW_FILTER_MAX = 512

# 原生字节序（'=' + 小端宿主）下的 ABI 结构：
# struct can_frame { canid_t can_id; __u8 len; __u8 flags; __u8 res0; __u8 res1; __u8 data[8]; }
# can_filter 为两个 32 位（id + mask）；错误过滤器与 RX 溢出计数为单个 32 位。
CAN_FRAME_STRUCT = struct.Struct("=IB3x8s")
CAN_FILTER_STRUCT = struct.Struct("=II")
CAN_ERR_FILTER_STRUCT = struct.Struct("=I")
RXQ_OVFL_STRUCT = struct.Struct("=I")
# 一条经典 CAN 记录的 ABI 长度（16 字节）；unpack 据此区分短记录与 CAN-FD 记录。
CAN_FRAME_SIZE = CAN_FRAME_STRUCT.size
# Linux 的 SocketCAN 常量在 Windows 上不存在，但保留其 ABI 值
# 可以让注入的假 socket 在那里演练纯组帧逻辑。
AF_CAN = getattr(socket, "AF_CAN", 29)
CAN_RAW = getattr(socket, "CAN_RAW", 1)
SOL_CAN_RAW = getattr(socket, "SOL_CAN_RAW", 101)
CAN_RAW_LOOPBACK = getattr(socket, "CAN_RAW_LOOPBACK", 1)
CAN_RAW_RECV_OWN_MSGS = getattr(socket, "CAN_RAW_RECV_OWN_MSGS", 2)
CAN_RAW_FILTER = getattr(socket, "CAN_RAW_FILTER", 1)
# SO_TIMESTAMPNS：为每条接收记录附加内核纳秒时间戳（辅助数据）。
SO_TIMESTAMPNS = getattr(socket, "SO_TIMESTAMPNS", 35)
SCM_TIMESTAMPNS = getattr(socket, "SCM_TIMESTAMPNS", SO_TIMESTAMPNS)
# SO_RXQ_OVFL：内核 socket 接收队列溢出计数（辅助数据中的 32 位计数器）。
SO_RXQ_OVFL = getattr(socket, "SO_RXQ_OVFL", 40)
SCM_RXQ_OVFL = SO_RXQ_OVFL
CAN_RAW_ERR_FILTER = getattr(socket, "CAN_RAW_ERR_FILTER", 2)
# 在受支持的 amd64 镜像上，Linux ``struct timespec``
# 由两个有符号 64 位字段组成。
# 显式保留线缆布局，以免 Windows 主机把 8 字节的
# 测试固定装置（test fixture）误当成完整时间戳。
_TIMESPEC_STRUCT = struct.Struct("=qq")
# 三个高位标志的组合掩码，用于校验标准帧不携带越界 ID 位。
_CAN_FLAG_MASK = CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG


class SocketCANError(CanTransportError):
    """某个 SocketCAN 操作因环境或链路原因失败。"""


class SocketCANFrameError(CanTransportFrameError):
    """原始 SocketCAN 记录格式不合法，在 Wire V1 解码前即被拒绝。"""


@dataclass(frozen=True)
class SocketCANFilter:
    """带类型的 SocketCAN 原始过滤器。

    ``mask`` 作用于仲裁 ID 位。帧类型标志会自动并入内核掩码，使标准过滤器
    不会意外吞掉扩展帧或 RTR 帧。错误帧单独使用 ``CAN_RAW_ERR_FILTER``:
    在 ``can_filter.can_id`` 中，Linux 为 ``CAN_ERR_FLAG`` 与 ``CAN_INV_FILTER``
    分配了相同的位值。
    """

    arbitration_id: int
    mask: int
    # 过滤器是否只匹配 extended 29 位帧；Wire V1 全部使用标准 11 位帧。
    is_extended_id: bool = False
    # 过滤器是否匹配 RTR 帧；Wire V1 拒绝远程帧，协议过滤器从不设置。
    is_remote_frame: bool = False

    def __post_init__(self) -> None:
        # 校验 ID 宽度与类型；越界即拒绝（失败即拒绝，不静默截断）。
        if type(self.arbitration_id) is not int or self.arbitration_id < 0:
            raise ValueError("filter arbitration_id must be a non-negative integer")
        if type(self.mask) is not int or self.mask < 0:
            raise ValueError("filter mask must be a non-negative integer")
        for name in ("is_extended_id", "is_remote_frame"):
            if type(getattr(self, name)) is not bool:
                raise TypeError(f"filter {name} must be a bool")
        maximum = CAN_EFF_MASK if self.is_extended_id else CAN_SFF_MASK
        if self.arbitration_id > maximum or self.mask > maximum:
            raise ValueError("filter arbitration_id and mask exceed the selected CAN id width")

    @property
    def raw_can_id(self) -> int:
        """带类型标志位的原始 can_id（过滤器 ABI 值）。"""

        value = self.arbitration_id
        if self.is_extended_id:
            value |= CAN_EFF_FLAG
        if self.is_remote_frame:
            value |= CAN_RTR_FLAG
        return value

    @property
    def raw_can_mask(self) -> int:
        # 在内核过滤器 ABI 中，CAN_ERR_FLAG 同时也是 CAN_INV_FILTER。
        # 它只能通过 CAN_RAW_ERR_FILTER 用于错误帧。
        # 掩码并入 EFF/RTR 位：标准过滤器绝不吞掉扩展帧或 RTR 帧。
        return self.mask | CAN_EFF_FLAG | CAN_RTR_FLAG

    def pack(self) -> bytes:
        """打包为内核 can_filter 的 8 字节 ABI 表示。"""

        return CAN_FILTER_STRUCT.pack(self.raw_can_id, self.raw_can_mask)


def pack_socketcan_frame(frame: CanFrame) -> bytes:
    """按 Linux ``struct can_frame`` 布局编码 1 个经典 CAN 帧。

    编码前完成完整边界校验：类型、DLC 范围（0..8）、ID 宽度、RTR 帧不得
    携带载荷、数据长度必须与 DLC 一致、raw_can_id 必须与标志位一致。
    任何不合法输入抛出 :class:`SocketCANFrameError`（失败即拒绝）。
    """

    if not isinstance(frame, CanFrame):
        raise SocketCANFrameError("SocketCAN send requires a CanFrame")
    if not isinstance(frame.data, bytes):
        raise SocketCANFrameError("SocketCAN payload must be immutable bytes")
    if type(frame.is_extended_id) is not bool:
        raise SocketCANFrameError("is_extended_id must be a bool")
    if type(frame.is_remote_frame) is not bool:
        raise SocketCANFrameError("is_remote_frame must be a bool")
    if type(frame.is_error_frame) is not bool:
        raise SocketCANFrameError("is_error_frame must be a bool")
    # 错误帧与 extended/RTR 标志互斥（内核 ABI 约束）。
    if frame.is_error_frame and (frame.is_extended_id or frame.is_remote_frame):
        raise SocketCANFrameError("CAN error frames cannot also be extended or remote frames")
    if frame.dlc is not None and (type(frame.dlc) is not int or not 0 <= frame.dlc <= CAN_MAX_DLC):
        raise SocketCANFrameError("CAN DLC must be an integer from 0 through 8")
    dlc = frame.effective_dlc
    if type(dlc) is not int or not 0 <= dlc <= CAN_MAX_DLC:
        raise SocketCANFrameError("CAN DLC must be an integer from 0 through 8")

    # ID 宽度：extended/错误帧允许 29 位，标准帧仅 11 位。
    maximum = CAN_EFF_MASK if frame.is_extended_id or frame.is_error_frame else CAN_SFF_MASK
    if type(frame.arbitration_id) is not int or not 0 <= frame.arbitration_id <= maximum:
        raise SocketCANFrameError("arbitration_id exceeds the selected CAN id width")
    if frame.is_remote_frame:
        # RTR 帧只有 DLC、没有数据载荷。
        if frame.data:
            raise SocketCANFrameError("remote frames must not carry a payload")
    elif len(frame.data) != dlc:
        raise SocketCANFrameError("CAN payload length must match DLC")

    # 组合 raw can_id：仲裁标识符加 EFF/RTR/ERR 高位标志。
    raw_can_id = frame.arbitration_id
    if frame.is_extended_id:
        raw_can_id |= CAN_EFF_FLAG
    if frame.is_remote_frame:
        raw_can_id |= CAN_RTR_FLAG
    if frame.is_error_frame:
        raw_can_id |= CAN_ERR_FLAG
    if frame.raw_can_id is not None and (type(frame.raw_can_id) is not int or not 0 <= frame.raw_can_id <= 0xFFFFFFFF):
        raise SocketCANFrameError("raw CAN ID must be a 32-bit unsigned integer when present")
    if frame.raw_can_id is not None and frame.raw_can_id != raw_can_id:
        raise SocketCANFrameError("raw CAN ID does not match arbitration ID and frame flags")
    # 载荷补零到 8 字节（struct can_frame.data 固定 8 字节）。
    payload = frame.data.ljust(CAN_MAX_DLC, b"\x00")
    return CAN_FRAME_STRUCT.pack(raw_can_id, dlc, payload)


def unpack_socketcan_frame(
    payload: bytes,
    *,
    kernel_timestamp_ns: int | None = None,
    kernel_drop_count: int | None = None,
    observed_monotonic_ts: float | None = None,
    observed_wall_ts: float | None = None,
) -> CanFrame:
    """解码 1 条完整的经典 CAN 记录，并保留其帧标志。

    短记录、CAN-FD 尺寸记录、无效 DLC、错误/extended/RTR 矛盾组合与越界
    ID 位都抛出 :class:`SocketCANFrameError`（在 Wire V1 解码之前拒绝）。
    成功返回不可变 :class:`CanFrame`，保留 raw can_id 与全部观测元数据。
    """

    if not isinstance(payload, bytes):
        raise SocketCANFrameError("SocketCAN receive payload must be bytes")
    if len(payload) != CAN_FRAME_SIZE:
        # 更长的记录是 CAN-FD 布局（或畸形），不支持；更短的是截断记录。
        if len(payload) > CAN_FRAME_SIZE:
            raise SocketCANFrameError(
                f"unsupported SocketCAN frame layout: expected {CAN_FRAME_SIZE} bytes, got {len(payload)}"
            )
        raise SocketCANFrameError(f"short SocketCAN frame: expected {CAN_FRAME_SIZE} bytes, got {len(payload)}")
    raw_can_id, dlc, raw_data = CAN_FRAME_STRUCT.unpack(payload)
    # Classic CAN 的 DLC 最大为 8；9..15 是 CAN-FD 尺寸记录，拒绝。
    if dlc > CAN_MAX_DLC:
        raise SocketCANFrameError(f"SocketCAN DLC {dlc} exceeds classic CAN maximum {CAN_MAX_DLC}")

    is_error_frame = bool(raw_can_id & CAN_ERR_FLAG)
    is_extended_id = bool(raw_can_id & CAN_EFF_FLAG)
    is_remote_frame = bool(raw_can_id & CAN_RTR_FLAG)
    # 内核 ABI：错误帧绝不与 extended/RTR 标志同置。
    if is_error_frame and (is_extended_id or is_remote_frame):
        raise SocketCANFrameError("SocketCAN error records cannot also be extended or remote frames")
    if is_error_frame:
        # 错误帧：低 29 位是 CAN_ERR_* 错误状态（含 bus-off 位 0x40）。
        arbitration_id = raw_can_id & CAN_ERR_MASK
    elif is_extended_id:
        arbitration_id = raw_can_id & CAN_EFF_MASK
    else:
        # 标准帧：除三个标志位与 11 位 ID 外不得有任何其他位。
        if raw_can_id & ~(CAN_SFF_MASK | _CAN_FLAG_MASK):
            raise SocketCANFrameError("standard SocketCAN record contains out-of-range CAN ID bits")
        arbitration_id = raw_can_id & CAN_SFF_MASK
    # RTR 帧无数据；数据帧截取 DLC 长度。
    data = b"" if is_remote_frame else bytes(raw_data[:dlc])
    if kernel_timestamp_ns is not None and (type(kernel_timestamp_ns) is not int or kernel_timestamp_ns < 0):
        raise SocketCANFrameError("kernel timestamp must be a non-negative integer")
    if kernel_drop_count is not None and (
        type(kernel_drop_count) is not int or not 0 <= kernel_drop_count <= 0xFFFFFFFF
    ):
        raise SocketCANFrameError("kernel RX drop count must be a 32-bit unsigned integer")
    for name, value in (
        ("observed_monotonic_ts", observed_monotonic_ts),
        ("observed_wall_ts", observed_wall_ts),
    ):
        if value is not None and (
            isinstance(value, bool) or not isinstance(value, int | float) or not math.isfinite(value)
        ):
            raise SocketCANFrameError(f"{name} must be finite when present")
    return CanFrame(
        arbitration_id=arbitration_id,
        data=data,
        is_extended_id=is_extended_id,
        is_remote_frame=is_remote_frame,
        is_error_frame=is_error_frame,
        dlc=dlc,
        kernel_timestamp_ns=kernel_timestamp_ns,
        kernel_drop_count=kernel_drop_count,
        observed_monotonic_ts=None if observed_monotonic_ts is None else float(observed_monotonic_ts),
        observed_wall_ts=None if observed_wall_ts is None else float(observed_wall_ts),
        raw_can_id=raw_can_id,
    )


class SocketCANTransport:
    """1 个受限的、同步的 AF_CAN/CAN_RAW 传输端口。

    构造过程无副作用。``open`` 只持有 1 个原始 socket；``receive`` 先 ``poll``
    再 ``recvmsg``，从不创建后台 worker 或适配器本地队列。CAN 总线重启仍属于
    CAN 核心/网络管理员的运维操作；可选的恢复探针（recovery probe）用于告知
    适配器：操作员或监管进程已完成该操作。
    """

    def __init__(
        self,
        interface: str,
        *,
        source: str = "socketcan",
        filters: Iterable[SocketCANFilter] | None = None,
        receive_own_messages: bool = False,
        loopback: bool = True,
        receive_buffer_bytes: int = CAN_FRAME_SIZE * 64,
        error_filter: int | None = CAN_ERR_MASK,
        require_kernel_timestamp: bool = True,
        recovery_probe: Callable[[], bool] | None = None,
        socket_factory: Callable[..., Any] = socket.socket,
        poller_factory: Callable[[], Any] | None = None,
        monotonic_clock: Callable[[], float] = time.monotonic,
        wall_clock: Callable[[], float] = time.time,
    ) -> None:
        # 构造无副作用：只校验参数并保存配置，绝不打开 socket 或硬件设备。
        self._interface = _validate_name(interface, "interface")
        self._source = _validate_name(source, "source")
        if filters is None:
            # 默认只放行 Wire V1 的三个入站仲裁标识符（ACK/STOP_ACK/遥测）。
            filters = (
                SocketCANFilter(MCU_CAN_ID_ACK, CAN_SFF_MASK),
                SocketCANFilter(MCU_CAN_ID_STOP_ACK, CAN_SFF_MASK),
                SocketCANFilter(MCU_CAN_ID_TELEMETRY, CAN_SFF_MASK),
            )
        try:
            # 限量取值：超过内核上限的过滤器列表在构造期即失败（失败即拒绝）。
            self._filters = tuple(islice(iter(filters), CAN_RAW_FILTER_MAX + 1))
        except TypeError as exc:
            raise TypeError("filters must be an iterable of SocketCANFilter values") from exc
        if len(self._filters) > CAN_RAW_FILTER_MAX:
            raise ValueError(f"filters cannot exceed the Linux CAN_RAW limit of {CAN_RAW_FILTER_MAX}")
        if any(not isinstance(item, SocketCANFilter) for item in self._filters):
            raise TypeError("filters must contain SocketCANFilter values")
        for name, value in (("receive_own_messages", receive_own_messages), ("loopback", loopback)):
            if type(value) is not bool:
                raise TypeError(f"{name} must be a bool")
        if type(receive_buffer_bytes) is not int or receive_buffer_bytes < CAN_FRAME_SIZE:
            raise ValueError(f"receive_buffer_bytes must be at least {CAN_FRAME_SIZE} bytes")
        if error_filter is not None and (type(error_filter) is not int or not 0 <= error_filter <= CAN_ERR_MASK):
            raise ValueError("error_filter must be a 29-bit mask when present")
        if type(require_kernel_timestamp) is not bool:
            raise TypeError("require_kernel_timestamp must be a bool")
        if recovery_probe is not None and not callable(recovery_probe):
            raise TypeError("recovery_probe must be callable when present")
        if not callable(socket_factory):
            raise TypeError("socket_factory must be callable")
        if poller_factory is not None and not callable(poller_factory):
            raise TypeError("poller_factory must be callable when present")
        if not callable(monotonic_clock) or not callable(wall_clock):
            raise TypeError("clock arguments must be callable")
        # 全部工厂/时钟注入点：测试用假 socket/poller/时钟演练纯逻辑。
        self._receive_own_messages = receive_own_messages
        self._loopback = loopback
        self._receive_buffer_bytes = receive_buffer_bytes
        self._error_filter = error_filter
        self._require_kernel_timestamp = require_kernel_timestamp
        self._recovery_probe = recovery_probe
        self._socket_factory = socket_factory
        self._poller_factory = poller_factory
        self._monotonic_clock = monotonic_clock
        self._wall_clock = wall_clock
        # 单锁保护 socket 句柄的打开/关闭/读取；I/O 本身在锁外执行。
        self._lock = threading.RLock()
        self._socket: Any | None = None

    @property
    def interface(self) -> str:
        """绑定的 SocketCAN 接口名（如 can0）。"""

        return self._interface

    @property
    def source(self) -> str:
        """入口来源标签（配合适配器 CanTransportConfig.source 使用）。"""

        return self._source

    @property
    def is_open(self) -> bool:
        """socket 是否已打开（锁内读取）。"""

        with self._lock:
            return self._socket is not None

    def open(self) -> None:
        """打开并配置唯一的 AF_CAN/CAN_RAW socket。

        步骤：创建 socket -> 设置接收缓冲与 SO_RXQ_OVFL -> loopback/回读 ->
        CAN_RAW_FILTER（类型化掩码）-> 可选 CAN_RAW_ERR_FILTER -> 可选
        SO_TIMESTAMPNS -> bind(接口) -> 非阻塞。任何一步失败即关闭候选
        socket 并抛出 SocketCANError；重复调用是幂等 no-op。
        """

        with self._lock:
            if self._socket is not None:
                return
            # 真实 socket 工厂需要平台提供 AF_CAN/CAN_RAW；注入工厂跳过该检查。
            if self._socket_factory is socket.socket and (
                not hasattr(socket, "AF_CAN") or not hasattr(socket, "CAN_RAW")
            ):
                raise SocketCANError("this platform does not expose AF_CAN/CAN_RAW")
            _resolve_poll_api(self._poller_factory)
            candidate = None
            try:
                candidate = self._socket_factory(AF_CAN, socket.SOCK_RAW, CAN_RAW)
                # 有界接收缓冲 + 内核接收溢出计数（每字节可观测的丢弃证据）。
                candidate.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self._receive_buffer_bytes)
                candidate.setsockopt(socket.SOL_SOCKET, SO_RXQ_OVFL, 1)
                candidate.setsockopt(SOL_CAN_RAW, CAN_RAW_LOOPBACK, int(self._loopback))
                candidate.setsockopt(
                    SOL_CAN_RAW,
                    CAN_RAW_RECV_OWN_MSGS,
                    int(self._receive_own_messages),
                )
                # 类型化内核过滤器：掩码含 EFF/RTR 位，帧类型变体不可能误匹配。
                candidate.setsockopt(
                    SOL_CAN_RAW,
                    CAN_RAW_FILTER,
                    b"".join(item.pack() for item in self._filters),
                )
                if self._error_filter is not None:
                    # 错误帧掩码与协议帧过滤器分离（CAN_RAW_ERR_FILTER）。
                    candidate.setsockopt(
                        SOL_CAN_RAW,
                        CAN_RAW_ERR_FILTER,
                        CAN_ERR_FILTER_STRUCT.pack(self._error_filter),
                    )
                if self._require_kernel_timestamp:
                    candidate.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
                candidate.bind((self._interface,))
                # 非阻塞：配合 poll() 使用，每次调用一条记录。
                candidate.setblocking(False)
            except (AttributeError, OSError, TypeError, ValueError) as exc:
                # 失败路径：关闭候选 socket，绝不泄漏文件描述符。
                try:
                    if candidate is not None:
                        candidate.close()
                except (AttributeError, OSError):
                    pass
                raise SocketCANError(f"failed to open SocketCAN interface {self._interface!r}: {exc}") from exc
            self._socket = candidate

    def close(self) -> None:
        """幂等关闭 socket；未打开时为 no-op。"""

        with self._lock:
            candidate = self._socket
            self._socket = None
        if candidate is None:
            return
        try:
            candidate.close()
        except (AttributeError, OSError) as exc:
            raise SocketCANError(f"failed to close SocketCAN interface {self._interface!r}: {exc}") from exc

    def send(self, frame: CanFrame) -> None:
        """编码并发送 1 帧；成功仅表示本地写出，绝不表示 MCU 已接受。

        OSError 按 errno 映射：EAGAIN/EWOULDBLOCK/ENOBUFS 为背压，
        网络类错误为链路丢失，其余为通用 SocketCANError（见 _map_socket_error）。
        """

        payload = pack_socketcan_frame(frame)
        candidate = self._require_socket()
        try:
            sent = candidate.send(payload)
        except OSError as exc:
            raise _map_socket_error(exc, operation="send") from exc
        if sent != len(payload):
            raise SocketCANError(f"SocketCAN short write: expected {len(payload)} bytes, sent {sent}")

    def receive(self, timeout_s: float) -> CanFrame | None:
        """poll + 非阻塞 recvmsg 读取 1 条记录；超时返回 None。

        记录校验顺序：poll 事件（HUP/ERR 即链路丢失）-> recvmsg 记录形状 ->
        MSG_TRUNC/MSG_CTRUNC/错误队列 -> 辅助数据（SO_TIMESTAMPNS 与
        SO_RXQ_OVFL）-> unpack_socketcan_frame 的帧级校验。畸形记录抛出
        SocketCANFrameError（可观测拒绝），读不到数据返回 None。
        """

        if (
            isinstance(timeout_s, bool)
            or not isinstance(timeout_s, int | float)
            or not math.isfinite(timeout_s)
            or timeout_s < 0
        ):
            raise ValueError("timeout_s must be a finite non-negative number")
        candidate = self._require_socket()
        poller_factory, poll_input_mask, poll_error_mask = _resolve_poll_api(self._poller_factory)
        poller = poller_factory()
        try:
            # 同时监听输入与错误事件；超时上限钳到毫秒整数可表示范围。
            poller.register(candidate, poll_input_mask | poll_error_mask)
            events = poller.poll(min(math.ceil(float(timeout_s) * 1000), 2_147_483_647))
        except OSError as exc:
            raise _map_socket_error(exc, operation="poll") from exc
        except (AttributeError, TypeError, ValueError) as exc:
            raise SocketCANError(f"SocketCAN poll failed: {exc}") from exc
        if not events:
            return None
        event_mask = 0
        candidate_fd = _socket_fileno(candidate)
        for event in events:
            # 逐事件校验：畸形事件记录即失败（失败即拒绝）。
            if not isinstance(event, tuple) or len(event) != 2:
                raise SocketCANError("SocketCAN poll returned an invalid event record")
            file_descriptor, mask = event
            if type(file_descriptor) is not int or type(mask) is not int:
                raise SocketCANError("SocketCAN poll returned invalid descriptor or event mask")
            if candidate_fd is not None and file_descriptor != candidate_fd:
                continue
            if mask < 0:
                raise SocketCANError("SocketCAN poll returned an invalid event mask")
            event_mask |= mask
        if not event_mask & poll_input_mask:
            # 无输入但有错误事件：接口故障按链路丢失报告。
            if event_mask & poll_error_mask:
                raise CanLinkLostError(f"SocketCAN interface {self._interface!r} reported poll error 0x{event_mask:x}")
            return None

        try:
            received = candidate.recvmsg(CAN_FRAME_SIZE, 256)
        except BlockingIOError:
            # poll 与 recvmsg 之间的竞争：视为无数据。
            return None
        except OSError as exc:
            if exc.errno in {errno.EAGAIN, errno.EWOULDBLOCK}:
                return None
            raise _map_socket_error(exc, operation="receive") from exc
        if not isinstance(received, tuple) or len(received) != 4:
            raise SocketCANFrameError("SocketCAN recvmsg returned an invalid record shape")
        raw, ancillary, message_flags, _address = received
        if not isinstance(raw, bytes):
            raise SocketCANFrameError("SocketCAN receive payload must be bytes")
        if not isinstance(message_flags, int):
            raise SocketCANFrameError("SocketCAN recvmsg returned an invalid message flag set")
        # 截断标记（数据/辅助数据）与错误队列记录在帧解码前拒绝。
        if message_flags & (getattr(socket, "MSG_TRUNC", 0) | getattr(socket, "MSG_CTRUNC", 0)):
            raise SocketCANFrameError("SocketCAN receive record was truncated")
        if message_flags & getattr(socket, "MSG_ERRQUEUE", 0):
            raise SocketCANFrameError("SocketCAN receive returned an error-queue record")
        kernel_timestamp_ns, kernel_drop_count = _extract_ancillary_metadata(ancillary)
        if self._require_kernel_timestamp and kernel_timestamp_ns is None:
            raise SocketCANFrameError("SocketCAN receive record did not contain SO_TIMESTAMPNS")
        # 主机观测时钟：提供时的单调与墙上时间（由适配器做倒退校验）。
        observed_monotonic_ts = _read_clock(self._monotonic_clock, "monotonic")
        observed_wall_ts = _read_clock(self._wall_clock, "wall")
        return unpack_socketcan_frame(
            raw,
            kernel_timestamp_ns=kernel_timestamp_ns,
            kernel_drop_count=kernel_drop_count,
            observed_monotonic_ts=observed_monotonic_ts,
            observed_wall_ts=observed_wall_ts,
        )

    def recover(self) -> bool:
        """若已配置，则确认外部完成的 CAN 核心重启。"""

        if not self.is_open or self._recovery_probe is None:
            return False
        try:
            result = self._recovery_probe()
            if type(result) is not bool:
                raise TypeError("SocketCAN recovery probe must return bool")
            return result
        except Exception as exc:
            raise SocketCANError(f"SocketCAN recovery probe failed: {exc}") from exc

    def __enter__(self) -> SocketCANTransport:
        """上下文管理器入口：open 并返回自身。"""

        self.open()
        return self

    def __exit__(self, _exc_type: object, _exc_value: object, _traceback: object) -> None:
        """上下文管理器出口：关闭 socket。"""

        self.close()

    def _require_socket(self) -> Any:
        """返回已打开的 socket；未打开即按链路丢失拒绝（锁内读取）。"""

        with self._lock:
            if self._socket is None:
                raise CanLinkLostError(f"SocketCAN interface {self._interface!r} is not open")
            return self._socket


def _validate_name(value: str, name: str) -> str:
    """校验接口/来源名：非空、已去空白、不含 NUL；非法即抛 ValueError。"""

    if not isinstance(value, str) or not value.strip() or value != value.strip() or "\x00" in value:
        raise ValueError(f"{name} must be a non-empty, trimmed string without NUL")
    return value


def _resolve_poll_api(poller_factory: Callable[[], Any] | None) -> tuple[Callable[[], Any], int, int]:
    """解析 poll 工厂与事件位掩码（输入位、错误位组合）。"""

    resolved_factory = poller_factory if poller_factory is not None else getattr(select, "poll", None)
    # POSIX poll 位值稳定；在 select 模块缺少一个或多个常量的平台上，
    # 用它们支持注入的 poller。
    poll_input_mask = getattr(select, "POLLIN", 0x001)
    poll_error_masks = tuple(
        getattr(select, name, fallback)
        for name, fallback in (
            ("POLLERR", 0x008),
            ("POLLHUP", 0x010),
            ("POLLNVAL", 0x020),
        )
    )
    if (
        not callable(resolved_factory)
        or type(poll_input_mask) is not int
        or any(type(mask) is not int for mask in poll_error_masks)
    ):
        raise SocketCANError("this platform does not expose select.poll SocketCAN support")
    return resolved_factory, poll_input_mask, poll_error_masks[0] | poll_error_masks[1] | poll_error_masks[2]


def _socket_fileno(candidate: Any) -> int | None:
    """读取 socket 的文件描述符；不可用（假 socket）时返回 None。"""

    try:
        value = candidate.fileno()
    except (AttributeError, OSError, TypeError, ValueError):
        return None
    return value if type(value) is int else None


def _read_clock(clock: Callable[[], float], name: str) -> float:
    """读取注入时钟；异常/非有限值按帧错误拒绝（失败即拒绝）。"""

    try:
        value = clock()
    except Exception as exc:
        raise SocketCANFrameError(f"{name} clock failed: {exc}") from exc
    if isinstance(value, bool) or not isinstance(value, int | float) or not math.isfinite(value):
        raise SocketCANFrameError(f"{name} clock returned a non-finite value")
    return float(value)


def _extract_ancillary_metadata(ancillary: object) -> tuple[int | None, int | None]:
    """从 recvmsg 辅助数据提取 (内核纳秒时间戳, RX 溢出计数)。

    只处理 SOL_SOCKET 层的 SO_TIMESTAMPNS 与 SO_RXQ_OVFL；重复时间戳/
    重复计数器、截断或畸形记录都抛出 SocketCANFrameError。
    """

    if not isinstance(ancillary, list | tuple):
        raise SocketCANFrameError("SocketCAN ancillary data has an invalid shape")
    timestamp: int | None = None
    kernel_drop_count: int | None = None
    for item in ancillary:
        if not isinstance(item, tuple) or len(item) != 3:
            raise SocketCANFrameError("SocketCAN ancillary record has an invalid shape")
        level, cmsg_type, data = item
        if level != socket.SOL_SOCKET:
            continue
        if cmsg_type in {SO_TIMESTAMPNS, SCM_TIMESTAMPNS}:
            if timestamp is not None:
                raise SocketCANFrameError("SocketCAN receive record contained duplicate timestamps")
            if not isinstance(data, bytes) or len(data) < _TIMESPEC_STRUCT.size:
                raise SocketCANFrameError("SocketCAN SO_TIMESTAMPNS ancillary data is truncated")
            # struct timespec = {秒, 纳秒}，两个有符号 64 位。
            seconds, nanoseconds = _TIMESPEC_STRUCT.unpack(data[: _TIMESPEC_STRUCT.size])
            if not 0 <= nanoseconds < 1_000_000_000:
                raise SocketCANFrameError("SocketCAN kernel timestamp has an invalid nanosecond field")
            timestamp = seconds * 1_000_000_000 + nanoseconds
            if timestamp < 0:
                raise SocketCANFrameError("SocketCAN kernel timestamp must be non-negative")
        elif cmsg_type in {SO_RXQ_OVFL, SCM_RXQ_OVFL}:
            if kernel_drop_count is not None:
                raise SocketCANFrameError("SocketCAN receive record contained duplicate RX drop counters")
            if not isinstance(data, bytes) or len(data) < RXQ_OVFL_STRUCT.size:
                raise SocketCANFrameError("SocketCAN SO_RXQ_OVFL ancillary data is truncated")
            (kernel_drop_count,) = RXQ_OVFL_STRUCT.unpack(data[: RXQ_OVFL_STRUCT.size])
    return timestamp, kernel_drop_count


def _map_socket_error(error: OSError, *, operation: str) -> CanTransportError:
    """按 errno 把 OSError 映射为类型化传输异常。

    EAGAIN/EWOULDBLOCK/ENOBUFS 是内核队列暂时满（背压，链路仍可用）；
    网络/设备类 errno 是链路丢失；其余为通用 SocketCANError。
    """

    if error.errno in {errno.EAGAIN, errno.EWOULDBLOCK, errno.ENOBUFS}:
        return CanTransportBackpressureError(f"SocketCAN {operation} temporarily backpressured: {error}")
    if error.errno in {
        errno.EBADF,
        errno.EHOSTDOWN,
        errno.EHOSTUNREACH,
        errno.EIO,
        errno.ENETDOWN,
        errno.ENETRESET,
        errno.ENODEV,
        errno.ENETUNREACH,
        errno.ENXIO,
        errno.ENOTCONN,
        errno.ENOLINK,
        errno.EPIPE,
        errno.ESHUTDOWN,
    }:
        return CanLinkLostError(f"SocketCAN {operation} lost the interface: {error}")
    return SocketCANError(f"SocketCAN {operation} failed: {error}")
