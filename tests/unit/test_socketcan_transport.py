"""SocketCANTransport 的单元测试（注入假 socket/poller/时钟，不开真实设备）。

覆盖：struct can_frame ABI 布局与 EFF/RTR/ERR 标志位、pack/unpack 拒绝边界、
过滤器打包契约、open 的 socket 选项序列、接收元数据（SO_TIMESTAMPNS 与
SO_RXQ_OVFL）、截断/畸形辅助数据拒绝、poll 错误即链路丢失、errno 到背压/
链路丢失的映射，以及非 Linux 平台上的导入与打开失败行为。
"""

import errno
import select
import socket
import struct
import subprocess
import sys
import textwrap
from collections import deque
from pathlib import Path

import pytest
from workbench.hardware import (
    CAN_EFF_FLAG,
    CAN_ERR_FLAG,
    CAN_ERR_MASK,
    CAN_FRAME_SIZE,
    CAN_RAW_ERR_FILTER,
    CAN_RTR_FLAG,
    CAN_SFF_MASK,
    SCM_TIMESTAMPNS,
    SO_RXQ_OVFL,
    SO_TIMESTAMPNS,
    CanFrame,
    CanLinkLostError,
    CanTransportBackpressureError,
    SocketCANFilter,
    SocketCANFrameError,
    SocketCANTransport,
    pack_socketcan_frame,
    unpack_socketcan_frame,
)
from workbench.hardware.socketcan_transport import (
    CAN_ERR_FILTER_STRUCT,
    CAN_FILTER_STRUCT,
    CAN_FRAME_STRUCT,
    SOL_CAN_RAW,
)

# POSIX poll 位值回退（与实现 _resolve_poll_api 一致）。
POLLIN = getattr(select, "POLLIN", 0x001)
POLLHUP = getattr(select, "POLLHUP", 0x010)

ROOT = Path(__file__).resolve().parents[2]
HARDWARE_PACKAGE = ROOT / "libs" / "hardware"


class FakeSocket:
    """记录 setsockopt/bind/send/recvmsg 的假 socket 固定装置。

    ``options`` 记录全部套接字选项；``incoming`` 是 (raw, ancillary, flags,
    address) 四元组队列；各类 *_error 为单发故障注入；fd 固定为 37 供
    poll 事件匹配。
    """

    def __init__(self) -> None:
        self.options: list[tuple[int, int, object]] = []
        self.bound: tuple[str, ...] | None = None
        self.blocking: bool | None = None
        self.sent: list[bytes] = []
        self.incoming: deque[tuple[bytes, list[tuple[int, int, bytes]], int, object]] = deque()
        self.send_error: OSError | None = None
        self.recv_error: OSError | None = None
        self.bind_error: OSError | None = None
        self.closed = False
        self.fd = 37

    def setsockopt(self, level: int, option: int, value: object) -> None:
        self.options.append((level, option, value))

    def bind(self, address: tuple[str, ...]) -> None:
        if self.bind_error is not None:
            raise self.bind_error
        self.bound = address

    def setblocking(self, value: bool) -> None:
        self.blocking = value

    def send(self, payload: bytes) -> int:
        if self.send_error is not None:
            error = self.send_error
            self.send_error = None
            raise error
        self.sent.append(payload)
        return len(payload)

    def recvmsg(self, _size: int, _ancillary_size: int):
        if self.recv_error is not None:
            error = self.recv_error
            self.recv_error = None
            raise error
        return self.incoming.popleft()

    def close(self) -> None:
        self.closed = True

    def fileno(self) -> int:
        return self.fd


class FakePoller:
    """记录注册与超时的假 poller 固定装置。

    默认按 socket 是否有入站数据返回 POLLIN；``forced_events`` 可强制返回
    指定事件（例如 POLLHUP 以演练链路丢失路径）。
    """

    def __init__(self, sock: FakeSocket) -> None:
        self.sock = sock
        self.registered: tuple[object, int] | None = None
        self.timeouts: list[int] = []
        self.forced_events: list[tuple[int, int]] | None = None

    def register(self, sock: object, mask: int) -> None:
        self.registered = (sock, mask)

    def poll(self, timeout_ms: int) -> list[tuple[int, int]]:
        self.timeouts.append(timeout_ms)
        if self.forced_events is not None:
            return self.forced_events
        return [(self.sock.fd, POLLIN)] if self.sock.incoming else []


def timestamp(seconds: int = 1_700_000_000, nanoseconds: int = 123_000_000) -> bytes:
    """打包 struct timespec 辅助数据（秒, 纳秒，两个有符号 64 位）。"""

    return struct.pack("=qq", seconds, nanoseconds)


def make_transport(
    fake_socket: FakeSocket,
    *,
    require_kernel_timestamp: bool = True,
    recovery_probe=None,
    clock_values: tuple[float, float] = (12.5, 1_700_000_000.5),
) -> tuple[SocketCANTransport, FakePoller]:
    """构造注入假 socket/poller/时钟的 SocketCANTransport 固定装置。"""

    poller = FakePoller(fake_socket)
    clock = iter(clock_values)
    transport = SocketCANTransport(
        "can0",
        source="usb-can-fd-prototype",
        require_kernel_timestamp=require_kernel_timestamp,
        recovery_probe=recovery_probe,
        socket_factory=lambda *_args: fake_socket,
        poller_factory=lambda: poller,
        monotonic_clock=lambda: next(clock),
        wall_clock=lambda: next(clock),
    )
    return transport, poller


# 意图：硬件包在缺少 Linux poll 原语的平台上仍可导入，open 报明确错误。
# 断言：子进程删除 select.poll/POLL* 后 import 成功；open 抛出含
#       "platform does not expose" 的 SocketCANError。
def test_hardware_package_import_survives_without_linux_poll_primitives() -> None:
    script = textwrap.dedent(
        f"""
        import select
        import sys

        sys.path.insert(0, {str(HARDWARE_PACKAGE)!r})
        for name in ("poll", "POLLIN", "POLLERR", "POLLHUP", "POLLNVAL"):
            if hasattr(select, name):
                delattr(select, name)

        import workbench.hardware as hardware

        transport = hardware.SocketCANTransport("can0")
        try:
            transport.open()
        except hardware.SocketCANError as exc:
            assert "platform does not expose" in str(exc)
        else:
            raise AssertionError("SocketCAN unexpectedly opened without select.poll support")
        """
    )
    result = subprocess.run([sys.executable, "-S", "-c", script], capture_output=True, text=True, check=False)

    assert result.returncode == 0, result.stderr


# 意图：pack/unpack 往返保留标准/extended/RTR/错误帧的全部标志与字段。
# 断言：每种帧编码后长度为 CAN_FRAME_SIZE；解码回 ID/数据/DLC/三个标志。
def test_pack_unpack_preserves_standard_extended_rtr_and_error_flags() -> None:
    # 数据表：标准帧、extended 29 位帧、无载荷 RTR 帧、错误帧。
    frames = (
        CanFrame(0x123, b"abc", dlc=3),
        CanFrame(0x1ABCDE, b"xyz", is_extended_id=True, dlc=3),
        CanFrame(0x123, b"", is_remote_frame=True, dlc=4),
        CanFrame(0x40, b"\x01", is_error_frame=True, dlc=1),
    )

    for original in frames:
        encoded = pack_socketcan_frame(original)
        assert len(encoded) == CAN_FRAME_SIZE
        decoded = unpack_socketcan_frame(encoded)
        assert decoded.arbitration_id == original.arbitration_id
        assert decoded.data == original.data
        assert decoded.effective_dlc == original.effective_dlc
        assert decoded.is_extended_id is original.is_extended_id
        assert decoded.is_remote_frame is original.is_remote_frame
        assert decoded.is_error_frame is original.is_error_frame


# 意图：编码后的 can_id 设置正确的 Linux 高位标志。
# 断言：标准帧为裸 ID；extended/RTR/错误帧分别为 ID | EFF_FLAG / RTR_FLAG / ERR_FLAG。
def test_socketcan_frame_layout_sets_the_linux_flag_bits() -> None:
    standard = int.from_bytes(pack_socketcan_frame(CanFrame(0x123, b"", dlc=0))[:4], "little")
    extended = int.from_bytes(pack_socketcan_frame(CanFrame(0x123, b"", is_extended_id=True, dlc=0))[:4], "little")
    remote = int.from_bytes(pack_socketcan_frame(CanFrame(0x123, b"", is_remote_frame=True, dlc=2))[:4], "little")
    error = int.from_bytes(pack_socketcan_frame(CanFrame(0x40, b"", is_error_frame=True, dlc=0))[:4], "little")
    assert standard == 0x123
    assert extended == CAN_EFF_FLAG | 0x123
    assert remote == CAN_RTR_FLAG | 0x123
    assert error == CAN_ERR_FLAG | 0x40


@pytest.mark.parametrize(
    "frame",
    [
        CanFrame(0x123, b"abc", dlc=2),
        CanFrame(0x123, b"", dlc=9),
        CanFrame(0x123, b"123", is_remote_frame=True, dlc=2),
        CanFrame(CAN_SFF_MASK + 1, b"", dlc=0),
    ],
)
# 意图：pack 拒绝歧义或不可表示的帧（失败即拒绝）。
# 断言：数据长度与 DLC 不符/DLC 越界/RTR 带载荷/标准 ID 越界均抛 SocketCANFrameError。
def test_pack_rejects_ambiguous_or_unrepresentable_frames(frame: CanFrame) -> None:
    with pytest.raises(SocketCANFrameError):
        pack_socketcan_frame(frame)


# 意图：unpack 拒绝短记录、CAN-FD 尺寸记录与无效 DLC。
# 断言：8 字节短记录报 short；72 字节报 unsupported；DLC=9 报 DLC 错误。
def test_unpack_rejects_short_fd_and_invalid_dlc_records() -> None:
    with pytest.raises(SocketCANFrameError, match="short"):
        unpack_socketcan_frame(b"\0" * 8)
    with pytest.raises(SocketCANFrameError, match="unsupported"):
        unpack_socketcan_frame(b"\0" * 72)
    invalid_dlc = CAN_FRAME_STRUCT.pack(0x123, 9, b"\0" * 8)
    with pytest.raises(SocketCANFrameError, match="DLC"):
        unpack_socketcan_frame(invalid_dlc)


# 意图：过滤器打包遵循内核 can_filter 标志/掩码契约。
# 断言：raw id 为裸 ID；掩码 = 全 11 位 | EFF | RTR（类型位不可能误匹配）；
#       错误掩码经 CAN_ERR_FILTER_STRUCT 原样往返。
def test_filter_packing_matches_socketcan_flag_mask_contract() -> None:
    item = SocketCANFilter(0x123, CAN_SFF_MASK)
    raw_id, raw_mask = CAN_FILTER_STRUCT.unpack(item.pack())
    assert raw_id == 0x123
    assert raw_mask == CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG

    error_mask = CAN_ERR_FILTER_STRUCT.unpack(CAN_ERR_FILTER_STRUCT.pack(CAN_ERR_MASK))[0]
    assert error_mask == CAN_ERR_MASK


# 意图：open 只创建一个原始 socket，接收保留内核时间戳元数据。
# 断言：绑定 can0、非阻塞；已启用 SO_TIMESTAMPNS/SO_RXQ_OVFL/CAN_RAW_ERR_FILTER；
#       接收帧的 ID/数据/DLC/内核时间戳/主机观测时钟/raw ID 正确；poll 超时 1 ms。
def test_open_uses_one_raw_socket_and_receive_preserves_timestamp_metadata() -> None:
    fake = FakeSocket()
    transport, poller = make_transport(fake)
    raw = CAN_FRAME_STRUCT.pack(0x180, 3, b"abc\0\0\0\0\0")
    fake.incoming.append((raw, [(socket.SOL_SOCKET, SCM_TIMESTAMPNS, timestamp())], 0, ()))

    assert not transport.is_open
    transport.open()
    assert transport.is_open
    assert fake.bound == ("can0",)
    assert fake.blocking is False
    assert (socket.SOL_SOCKET, SO_TIMESTAMPNS, 1) in fake.options
    assert (socket.SOL_SOCKET, SO_RXQ_OVFL, 1) in fake.options
    assert (
        SOL_CAN_RAW,
        CAN_RAW_ERR_FILTER,
        CAN_ERR_FILTER_STRUCT.pack(CAN_ERR_MASK),
    ) in fake.options

    frame = transport.receive(0.0001)
    assert frame is not None
    assert frame.arbitration_id == 0x180
    assert frame.data == b"abc"
    assert frame.effective_dlc == 3
    assert frame.kernel_timestamp_ns == 1_700_000_000_123_000_000
    assert frame.observed_monotonic_ts == 12.5
    assert frame.observed_wall_ts == 1_700_000_000.5
    assert frame.raw_can_id == 0x180
    assert poller.timeouts == [1]


# 意图：接收保留内核 RX 溢出计数与 extended 原始标志。
# 断言：extended 帧 is_extended_id 为真、raw_can_id 含 EFF 位、
#       kernel_drop_count=7、arbitration_id 剥离标志后为 0x1ABCDE。
def test_receive_preserves_kernel_drop_counter_and_raw_flags() -> None:
    fake = FakeSocket()
    transport, _poller = make_transport(fake)
    raw_id = CAN_EFF_FLAG | 0x1ABCDE
    raw = CAN_FRAME_STRUCT.pack(raw_id, 2, b"xy\0\0\0\0\0\0")
    fake.incoming.append(
        (
            raw,
            [
                (socket.SOL_SOCKET, SCM_TIMESTAMPNS, timestamp(1_700_000_001, 1)),
                (socket.SOL_SOCKET, 40, struct.pack("=I", 7)),
            ],
            0,
            (),
        )
    )

    transport.open()
    frame = transport.receive(0.0)

    assert frame is not None
    assert frame.is_extended_id
    assert frame.raw_can_id == raw_id
    assert frame.kernel_drop_count == 7
    assert frame.arbitration_id == 0x1ABCDE


# 意图：接收超时、close 与恢复探针都是有界且幂等的。
# 断言：超时返回 None 且 poll 超时 0 ms；恢复探针按返回值回传；close 可重复；
#       关闭后 receive 抛 CanLinkLostError。
def test_receive_timeout_close_and_recovery_are_bounded_and_idempotent() -> None:
    fake = FakeSocket()
    recovered = [False]
    transport, poller = make_transport(fake, recovery_probe=lambda: recovered[0])
    transport.open()
    assert transport.receive(0.0) is None
    assert poller.timeouts == [0]
    assert transport.recover() is False
    recovered[0] = True
    assert transport.recover() is True
    transport.close()
    transport.close()
    assert fake.closed
    assert not transport.is_open
    with pytest.raises(CanLinkLostError):
        transport.receive(0.0)


# 意图：默认要求内核时间戳，显式关闭后可缺省。
# 断言：缺失 SO_TIMESTAMPNS 时默认抛 SocketCANFrameError；
#       require_kernel_timestamp=False 时帧可接收且 kernel_timestamp_ns 为 None。
def test_receive_requires_kernel_timestamp_unless_explicitly_disabled() -> None:
    fake = FakeSocket()
    transport, _poller = make_transport(fake)
    transport.open()
    fake.incoming.append((CAN_FRAME_STRUCT.pack(0x180, 0, b"\0" * 8), [], 0, ()))
    with pytest.raises(SocketCANFrameError, match="SO_TIMESTAMPNS"):
        transport.receive(0.0)

    optional_fake = FakeSocket()
    optional, _optional_poller = make_transport(optional_fake, require_kernel_timestamp=False)
    optional.open()
    optional_fake.incoming.append((CAN_FRAME_STRUCT.pack(0x180, 0, b"\0" * 8), [], 0, ()))
    frame = optional.receive(0.0)
    assert frame is not None
    assert frame.kernel_timestamp_ns is None


@pytest.mark.parametrize(
    "ancillary",
    [
        [(socket.SOL_SOCKET, SCM_TIMESTAMPNS, b"\0" * 8)],
        [(socket.SOL_SOCKET, 40, b"\0" * 2)],
        [(socket.SOL_SOCKET, SCM_TIMESTAMPNS, timestamp()), (socket.SOL_SOCKET, SCM_TIMESTAMPNS, timestamp())],
    ],
)
# 意图：截断或歧义（重复）的辅助数据被拒绝。
# 断言：截断的时间戳/截断的溢出计数/重复时间戳均抛 SocketCANFrameError。
def test_receive_rejects_truncated_or_ambiguous_ancillary_metadata(ancillary: list[tuple[int, int, bytes]]) -> None:
    fake = FakeSocket()
    transport, _poller = make_transport(fake)
    fake.incoming.append((CAN_FRAME_STRUCT.pack(0x180, 0, b"\0" * 8), ancillary, 0, ()))
    transport.open()

    with pytest.raises(SocketCANFrameError):
        transport.receive(0.0)


# 意图：消息截断标记在帧解码之前被拒绝。
# 断言：MSG_TRUNC 记录抛 SocketCANFrameError（match=truncated）。
def test_receive_rejects_message_truncation_before_frame_decode() -> None:
    fake = FakeSocket()
    transport, _poller = make_transport(fake)
    fake.incoming.append(
        (
            CAN_FRAME_STRUCT.pack(0x180, 8, b"\0" * 8),
            [(socket.SOL_SOCKET, SCM_TIMESTAMPNS, timestamp())],
            socket.MSG_TRUNC,
            (),
        )
    )
    transport.open()

    with pytest.raises(SocketCANFrameError, match="truncated"):
        transport.receive(0.0)


# 意图：poll 挂起事件按链路丢失报告，且不尝试读取。
# 断言：POLLHUP 事件抛 CanLinkLostError（match=poll error）。
def test_poll_hangup_is_reported_as_link_loss_without_receiving() -> None:
    fake = FakeSocket()
    transport, poller = make_transport(fake)
    poller.forced_events = [(fake.fd, POLLHUP)]
    transport.open()

    with pytest.raises(CanLinkLostError, match="poll error"):
        transport.receive(0.0)


# 意图：open 失败时关闭候选 socket，绝不泄漏文件描述符。
# 断言：bind 失败抛含 failed to open SocketCAN 的异常；fake 已 close；is_open 为假。
def test_open_failure_closes_the_candidate_socket() -> None:
    fake = FakeSocket()
    fake.bind_error = OSError(errno.EADDRNOTAVAIL, "missing interface")
    transport, _poller = make_transport(fake)
    with pytest.raises(Exception, match="failed to open SocketCAN"):
        transport.open()
    assert fake.closed
    assert not transport.is_open


# 意图：send 的内核队列压力映射为背压，而不是误判链路丢失。
# 断言：ENOBUFS 抛 CanTransportBackpressureError；is_open 保持为真。
def test_send_maps_kernel_queue_pressure_without_claiming_link_loss() -> None:
    fake = FakeSocket()
    transport, _poller = make_transport(fake, require_kernel_timestamp=False)
    transport.open()
    fake.send_error = OSError(errno.ENOBUFS, "tx queue full")
    with pytest.raises(CanTransportBackpressureError):
        transport.send(CanFrame(0x123, b"", dlc=0))
    assert transport.is_open
