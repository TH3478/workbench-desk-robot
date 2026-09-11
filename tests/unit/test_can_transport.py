"""SafeCANBus / DeviceRuntime 的单元测试（注入 fake 传输与 fake 时钟）。

覆盖：Wire V1 解码拒绝边界、生命周期所有权、命令平面背压、ACK 关联与
重试/超时升级、STOP 抢占、bus-off 与恢复（不回放）、遥测半区间回绕排序、
失败即拒绝的时钟/帧错误路径，以及与并发 shutdown 的竞争。全部断言都不
依赖真实 SocketCAN 或硬件设备。
"""

import threading
from collections import deque
from dataclasses import FrozenInstanceError

import pytest
from workbench.hardware import (
    CAN_ERR_BUSOFF,
    CAN_ERR_FLAG,
    MCU_CAN_ID_ACK,
    MCU_CAN_ID_COMMAND,
    MCU_CAN_ID_STOP,
    MCU_CAN_ID_STOP_ACK,
    MCU_CAN_ID_TELEMETRY,
    CanBusOffError,
    CanDiagnosticCode,
    CanExternalRecord,
    CanFrame,
    CanFrameKind,
    CanLinkLostError,
    CanLinkState,
    CanReceiveStatus,
    CanSendStatus,
    CanTransportBackpressureError,
    CanTransportConfig,
    CanTransportEnvelope,
    CanTransportFrameError,
    DeviceRuntime,
    DeviceRuntimeState,
    SafeCANBus,
    decode_can_frame,
)


class FakeClock:
    """手动推进的单调时钟固定装置：now 只能向前拨，模拟期限与超时。"""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


class FakeTransport:
    """注入式 CanTransportPort 固定装置：记录 send/打开/关闭，可注入错误。

    ``send_error``/``receive_error`` 只触发一次后即清空（单发故障注入），
    ``incoming`` 是入站帧队列，``recover_result`` 控制恢复探针结果。
    """

    def __init__(self) -> None:
        self.opened = False
        self.closed = False
        self.sent: list[CanFrame] = []
        self.incoming: deque[CanFrame] = deque()
        self.send_error: Exception | None = None
        self.receive_error: Exception | None = None
        self.recover_result = True

    def open(self) -> None:
        self.opened = True

    def send(self, frame: CanFrame) -> None:
        if self.send_error is not None:
            error = self.send_error
            self.send_error = None
            raise error
        self.sent.append(frame)

    def receive(self, timeout_s: float) -> CanFrame | None:
        del timeout_s
        if self.receive_error is not None:
            error = self.receive_error
            self.receive_error = None
            raise error
        return self.incoming.popleft() if self.incoming else None

    def recover(self) -> bool:
        return self.recover_result

    def close(self) -> None:
        self.closed = True


class RecordingAdapter:
    """记录生命周期调用顺序的 DeviceAdapter 固定装置（全部成功）。"""

    def __init__(self) -> None:
        self.events: list[str] = []

    def configure(self) -> bool:
        self.events.append("configure")
        return True

    def activate(self) -> bool:
        self.events.append("activate")
        return True

    def poll(self, receive_timeout_s: float) -> None:
        del receive_timeout_s
        self.events.append("poll")

    def deactivate(self) -> bool:
        self.events.append("deactivate")
        return True

    def cleanup(self) -> bool:
        self.events.append("cleanup")
        return True


def can_frame(arbitration_id: int, payload: list[int], **flags: bool) -> CanFrame:
    """便捷构造 CanFrame：payload 转不可变 bytes，其余走关键字标志。"""

    return CanFrame(arbitration_id, bytes(payload), **flags)


def command(command_id: int = 1, opcode: int = 1, retry_count: int = 0) -> CanFrame:
    """构造 Wire V1 COMMAND（0x100）：0=版本，1..2=ID 大端，3=opcode，4=重试，5..7=零。"""

    return can_frame(MCU_CAN_ID_COMMAND, [0x10, command_id >> 8, command_id & 0xFF, opcode, retry_count, 0, 0, 0])


def stop(command_id: int = 0x8001, retry_count: int = 0) -> CanFrame:
    """构造 Wire V1 STOP（0x080）：opcode=5，command_id 落在 STOP 分区。"""

    return can_frame(MCU_CAN_ID_STOP, [0x10, command_id >> 8, command_id & 0xFF, 5, retry_count, 0, 0, 0])


def ack(command_id: int = 1, opcode: int = 1, retry_count: int = 0) -> CanFrame:
    """构造成功 ACK（0x101）：result=0，fault=0，mode=0（idle）。"""

    return can_frame(MCU_CAN_ID_ACK, [0x10, command_id >> 8, command_id & 0xFF, opcode, retry_count, 0, 0, 0])


def rejected_ack(command_id: int = 1, opcode: int = 1, retry_count: int = 0) -> CanFrame:
    """构造被拒绝的普通 ACK：result=1，fault=duplicate_frame(5)，mode=faulted(4)。"""

    return can_frame(MCU_CAN_ID_ACK, [0x10, command_id >> 8, command_id & 0xFF, opcode, retry_count, 1, 5, 4])


def stop_ack(command_id: int = 0x8001, retry_count: int = 0, accepted: bool = True) -> CanFrame:
    """构造 STOP_ACK（0x081）：接受 = (0,0,3=stopped)，拒绝 = (1,3=stop_rejected,4=faulted)。"""

    return can_frame(
        MCU_CAN_ID_STOP_ACK,
        [
            0x10,
            command_id >> 8,
            command_id & 0xFF,
            5,
            retry_count,
            0 if accepted else 1,
            0 if accepted else 3,
            3 if accepted else 4,
        ],
    )


def telemetry(sequence_no: int = 1, *, fault_code: int = 0, device_mode: int = 0) -> CanFrame:
    """构造遥测（0x180）：1..4=序号大端，5=fault，6=mode，7=保留零。"""

    return can_frame(
        MCU_CAN_ID_TELEMETRY,
        [0x10, *sequence_no.to_bytes(4, "big"), fault_code, device_mode, 0],
    )


def running_bus(
    transport: FakeTransport,
    clock: FakeClock,
    config: CanTransportConfig | None = None,
) -> SafeCANBus:
    """构造并同步启动（background=False）一个 SafeCANBus 固定装置。"""

    bus = SafeCANBus(transport, clock=clock, config=config)
    assert bus.start(background=False)
    return bus


# 意图：decode_can_frame 拒绝非 Wire 帧与违反跨字段语义的帧。
# 断言：合法命令解码为 COMMAND；bytearray 载荷/短载荷/未知 ID/extended 帧/
#       错误版本/保留字节非零/普通 ID 的 STOP/STOP 分区的 ACK/错误 mode 均抛 ValueError。
def test_decode_rejects_non_wire_frames_and_bad_cross_fields() -> None:
    valid = command()
    assert decode_can_frame(valid).kind is CanFrameKind.COMMAND
    # 数据表：逐个违反单一契约条款的畸形帧。
    cases = (
        CanFrame(MCU_CAN_ID_COMMAND, bytearray(valid.data)),
        CanFrame(MCU_CAN_ID_COMMAND, valid.data[:-1]),
        CanFrame(0x123, valid.data),
        CanFrame(MCU_CAN_ID_COMMAND, valid.data, is_extended_id=True),
        CanFrame(MCU_CAN_ID_COMMAND, bytes([0x11, *valid.data[1:]])),
        CanFrame(MCU_CAN_ID_COMMAND, bytes([0x10, 0, 1, 1, 0, 1, 0, 0])),
        stop(1),
        ack(0x8001),
        can_frame(MCU_CAN_ID_ACK, [0x10, 0, 1, 1, 0, 0, 0, 4]),
    )
    for frame in cases:
        with pytest.raises(ValueError):
            decode_can_frame(frame)


# 意图：send 需要运行时激活；命令平面饱和返回类型化背压。
# 断言：未启动为 NOT_RUNNING；容量 1 时第 2 条为 BACKPRESSURE、深度不变；
#       出站方向白名单外的遥测为 INVALID_FRAME。
def test_send_requires_lifecycle_and_queue_backpressure_is_typed() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = SafeCANBus(transport, clock=clock, config=CanTransportConfig(queue_capacity=1))

    assert bus.send(command()).status is CanSendStatus.NOT_RUNNING
    assert bus.start(background=False)
    assert bus.send(command(1)).status is CanSendStatus.QUEUED
    assert bus.send(command(2)).status is CanSendStatus.BACKPRESSURE
    assert bus.queued_count == 1
    assert bus.send(telemetry()).status is CanSendStatus.INVALID_FRAME


# 意图：适配器使用唯一 DeviceRuntime 承载生命周期、worker 与命令平面。
# 断言：NEW -> ACTIVE -> CLEANED 序列；worker 存活/退出；命令深度一致；shutdown 可重入。
def test_can_adapter_uses_one_runtime_for_lifecycle_worker_and_command_plane() -> None:
    transport = FakeTransport()
    bus = SafeCANBus(transport, clock=FakeClock())

    assert bus.runtime.state is DeviceRuntimeState.NEW
    assert not hasattr(bus, "_worker")
    assert bus.start(background=True)
    assert bus.runtime.state is DeviceRuntimeState.ACTIVE
    assert bus.runtime.worker_alive
    assert bus.runtime.command_depth == bus.queued_count

    assert bus.shutdown(timeout_s=1.0)
    assert bus.runtime.state is DeviceRuntimeState.CLEANED
    assert not bus.runtime.worker_alive
    assert bus.shutdown(timeout_s=0.0)


# 意图：DeviceRuntime 拥有适配器生命周期调用顺序。
# 断言：start/shutdown 后事件序列恰为 configure -> activate -> deactivate -> cleanup。
def test_device_runtime_owns_the_adapter_lifecycle_sequence() -> None:
    adapter = RecordingAdapter()
    runtime = DeviceRuntime(
        adapter,
        command_capacity=1,
        telemetry_capacity=1,
        health_capacity=1,
        max_subscribers_per_id=1,
        poll_interval_s=0.001,
    )

    assert runtime.start(background=False)
    assert runtime.state is DeviceRuntimeState.ACTIVE
    assert runtime.shutdown(timeout_s=1.0)
    assert runtime.state is DeviceRuntimeState.CLEANED
    assert adapter.events == ["configure", "activate", "deactivate", "cleanup"]


# 意图：worker 消费 1 条命令并恰好关联 1 次 ACK，重复 ACK 不再确认。
# 断言：service_once 分发命令并建立关联；ACK 到达 confirmed；重复 ACK 为 DUPLICATE、
#       不可暴露、外部投影仍只有 1 条。
def test_worker_drains_one_command_and_correlates_ack_once() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)

    assert bus.send(command(7)).accepted
    assert bus.service_once() is None
    assert transport.sent == [command(7)]

    transport.incoming.append(ack(7))
    result = bus.service_once()
    assert result is not None
    assert result.status is CanReceiveStatus.ACCEPTED
    assert result.confirmed
    assert bus.pending_command_id is None

    transport.incoming.append(ack(7))
    duplicate = bus.service_once()
    assert duplicate is not None
    assert duplicate.status is CanReceiveStatus.DUPLICATE
    assert duplicate.external_record is not None
    assert not duplicate.external_record.exposure_allowed
    assert len(bus.external_records()) == 1


# 意图：接受的入站事件产生不可变且受限的外部投影记录。
# 断言：记录字段（来源/接口/序号/事件类型/帧类型/DLC/内核时间戳/证据引用）完整；
#       数据十六进制一致；记录被发布进外部队列；dataclass 冻结不可改写。
def test_accepted_ingress_exposes_a_bounded_immutable_projection() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock(), CanTransportConfig(external_capacity=2))
    frame = telemetry(27)
    # 携带内核与主机双时间戳的遥测：时间戳来源应为 kernel+host。
    frame = CanFrame(
        frame.arbitration_id,
        frame.data,
        dlc=8,
        kernel_timestamp_ns=1_700_000_000_123_000_000,
        kernel_drop_count=4,
        observed_monotonic_ts=12.5,
        observed_wall_ts=1_700_000_000.5,
        raw_can_id=frame.arbitration_id,
    )
    transport.incoming.append(frame)

    result = bus.service_once()

    assert result is not None
    assert isinstance(result.external_record, CanExternalRecord)
    record = result.external_record
    assert record.source == "mcu-can"
    assert record.interface == "injected-can"
    assert record.ingress_sequence == 0
    assert record.event_type == "telemetry"
    assert record.frame_kind is CanFrameKind.TELEMETRY
    assert record.dlc == 8
    assert record.kernel_timestamp_ns == 1_700_000_000_123_000_000
    assert record.kernel_drop_count == 4
    assert record.timestamp_source == "kernel+host"
    assert record.exposure_allowed
    assert record.sequence_no == 27
    assert record.data_hex == frame.data.hex()
    assert record.to_dict()["evidence_refs"] == ["can-ingress://mcu-can/injected-can/0"]
    assert bus.external_records() == (record,)
    with pytest.raises(FrozenInstanceError):
        record.ingress_sequence = 4  # type: ignore[misc]


# 意图：外部投影容量满时淘汰最旧记录并记录背压诊断。
# 断言：容量 1 下两条遥测后只保留序号 2；丢弃计数 1；诊断含 EXTERNAL_BACKPRESSURE。
def test_external_projection_drops_oldest_record_and_records_backpressure() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock(), CanTransportConfig(external_capacity=1))
    transport.incoming.extend((telemetry(1), telemetry(2)))

    assert bus.service_once() is not None
    assert bus.service_once() is not None

    records = bus.external_records()
    assert len(records) == 1
    assert records[0].sequence_no == 2
    assert bus.external_drop_count == 1
    assert any(item.code is CanDiagnosticCode.EXTERNAL_BACKPRESSURE for item in bus.diagnostics())


# 意图：矛盾的 raw_can_id 被拒绝，且不污染外部投影。
# 断言：INVALID_FRAME；记录不可暴露、frame_valid 为假；矛盾 raw ID 清空为 None、
#       仲裁标识符仍保留。
def test_malformed_raw_id_is_rejected_without_corrupting_the_external_projection() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock())
    valid = telemetry(28)
    transport.incoming.append(CanFrame(valid.arbitration_id, valid.data, raw_can_id=0x123))

    result = bus.service_once()

    assert result is not None
    assert result.status is CanReceiveStatus.INVALID_FRAME
    assert result.external_record is not None
    assert not result.external_record.exposure_allowed
    assert not result.external_record.frame_valid
    assert result.external_record.raw_can_id is None
    assert result.external_record.arbitration_id == MCU_CAN_ID_TELEMETRY


# 意图：传输帧格式错误转换为可观测的有界拒绝，链路不被误判丢失。
# 断言：INVALID_FRAME；外部记录携带原因且不可暴露。
def test_transport_frame_error_is_an_observable_bounded_rejection() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock())
    transport.receive_error = CanTransportFrameError("truncated SocketCAN record")

    result = bus.service_once()

    assert result is not None
    assert result.status is CanReceiveStatus.INVALID_FRAME
    assert result.external_record is not None
    assert result.external_record.reason == "truncated SocketCAN record"
    assert not result.external_record.exposure_allowed


# 意图：内核发送背压在固定预算内重试同一命令。
# 断言：前 2 次 send 背压时命令仍在队列、链路保持 ACTIVE；第 3 次成功发送；
#       关联建立；诊断中恰有 2 条 TRANSPORT_BACKPRESSURE。
def test_transport_backpressure_retries_within_the_fixed_budget() -> None:
    class TwiceBackpressuredTransport(FakeTransport):
        def __init__(self) -> None:
            super().__init__()
            self.send_attempts = 0

        def send(self, frame: CanFrame) -> None:
            self.send_attempts += 1
            if self.send_attempts <= 2:
                raise CanTransportBackpressureError("kernel transmit queue is full")
            super().send(frame)

    transport = TwiceBackpressuredTransport()
    bus = running_bus(
        transport,
        FakeClock(),
        CanTransportConfig(transport_backpressure_budget=2),
    )
    assert bus.send(command(30)).accepted

    assert bus.service_once() is None
    assert bus.service_once() is None
    assert bus.state is CanLinkState.ACTIVE
    assert bus.queued_count == 1

    assert bus.service_once() is None
    assert transport.sent == [command(30)]
    assert bus.pending_command_id == 30
    assert [item.code for item in bus.diagnostics()].count(CanDiagnosticCode.TRANSPORT_BACKPRESSURE) == 2


# 意图：内核发送背压预算耗尽即失败关闭，且不回放过期流量。
# 断言：第 2 次背压后 LINK_LOST；命令平面清空、无在途 ID；后续 send 被拒；
#       诊断含 2 条背压与 1 条链路丢失。
def test_transport_backpressure_exhaustion_fails_closed_without_replay() -> None:
    class AlwaysBackpressuredTransport(FakeTransport):
        def send(self, frame: CanFrame) -> None:
            del frame
            raise CanTransportBackpressureError("kernel transmit queue is full")

    transport = AlwaysBackpressuredTransport()
    bus = running_bus(
        transport,
        FakeClock(),
        CanTransportConfig(transport_backpressure_budget=1),
    )
    assert bus.send(command(31)).accepted

    assert bus.service_once() is None
    assert bus.state is CanLinkState.ACTIVE
    assert bus.queued_count == 1

    assert bus.service_once() is None
    assert bus.state is CanLinkState.LINK_LOST
    assert bus.queued_count == 0
    assert bus.pending_command_id is None
    assert bus.send(command(32)).status is CanSendStatus.LINK_UNAVAILABLE
    codes = [item.code for item in bus.diagnostics()]
    assert codes.count(CanDiagnosticCode.TRANSPORT_BACKPRESSURE) == 2
    assert codes.count(CanDiagnosticCode.LINK_LOST) == 1


# 意图：bus-off 错误帧可观测但绝不进入外部投影；适配器进入 BUS_OFF。
# 断言：INVALID_FRAME；记录 health=BUS_OFF、is_error_frame=True、不可暴露；
#       链路 BUS_OFF；外部队列为空；诊断含 BUS_OFF。
def test_bus_off_error_frame_is_observable_but_never_externally_exposed() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock())
    transport.incoming.append(
        CanFrame(
            CAN_ERR_BUSOFF,
            b"\0" * 8,
            is_error_frame=True,
            dlc=8,
            raw_can_id=CAN_ERR_FLAG | CAN_ERR_BUSOFF,
        )
    )

    result = bus.service_once()

    assert result is not None
    assert result.status is CanReceiveStatus.INVALID_FRAME
    assert result.external_record is not None
    assert result.external_record.health is CanLinkState.BUS_OFF
    assert result.external_record.is_error_frame is True
    assert not result.external_record.frame_valid
    assert not result.external_record.exposure_allowed
    assert bus.state is CanLinkState.BUS_OFF
    assert bus.external_records() == ()
    assert any(item.code is CanDiagnosticCode.BUS_OFF for item in bus.diagnostics())


# 意图：时钟故障产生恰 1 条可观测拒绝，且不递归生成记录（避免自我触发）。
# 断言：INVALID_FRAME 原因含时钟故障；外部队列空；诊断恰 1 条 CLOCK_ROLLBACK；
#       链路 LINK_LOST。
def test_clock_failure_emits_one_rejection_without_recursive_record_generation() -> None:
    class BrokenClock:
        def __init__(self) -> None:
            self.calls = 0

        def monotonic(self) -> float:
            self.calls += 1
            if self.calls == 1:
                return 0.0
            raise RuntimeError("clock unavailable")

    transport = FakeTransport()
    bus = running_bus(transport, BrokenClock())
    transport.incoming.append(telemetry(29))

    result = bus.service_once()

    assert result is not None
    assert result.status is CanReceiveStatus.INVALID_FRAME
    assert result.external_record is not None
    assert result.external_record.reason == "monotonic clock failed: clock unavailable"
    assert bus.external_records() == ()
    assert bus.take_external_record() is None
    codes = [item.code for item in bus.diagnostics()]
    assert codes.count(CanDiagnosticCode.CLOCK_ROLLBACK) == 1
    assert bus.state is CanLinkState.LINK_LOST


# 意图：畸形入站 ACK 被拒绝但不清除在途命令关联。
# 断言：INVALID_FRAME、不确认；pending_command_id 仍为 8（畸形 ACK 不构成确认）。
def test_malformed_inbound_frame_is_rejected_without_clearing_pending_command() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)

    assert bus.send(command(8)).accepted
    assert bus.service_once() is None
    # mode=4(faulted) 与 result=0 矛盾：跨字段语义无效。
    transport.incoming.append(can_frame(MCU_CAN_ID_ACK, [0x10, 0, 8, 1, 0, 0, 0, 4]))
    malformed = bus.service_once()
    assert malformed is not None
    assert malformed.status is CanReceiveStatus.INVALID_FRAME
    assert not malformed.confirmed
    assert bus.pending_command_id == 8


# 意图：ACK 超时先按预算重试（保持关联），预算耗尽升级为关联 STOP。
# 断言：重试帧 retry_count=1；随后发出 STOP 且链路 STOPPING；STOP_ACK 确认后
#       SAFE_STOPPED 且 confirmed。
def test_retry_keeps_correlation_and_timeout_escalates_to_stop() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    config = CanTransportConfig(ack_timeout_s=1.0, ack_retry_budget=1, stop_timeout_s=1.0)
    bus = running_bus(transport, clock, config)

    assert bus.send(command(3)).accepted
    assert bus.service_once() is None
    clock.advance(1.0)
    assert bus.service_once() is None
    assert transport.sent[-1] == command(3, retry_count=1)

    clock.advance(1.0)
    assert bus.service_once() is None
    assert transport.sent[-1].arbitration_id == MCU_CAN_ID_STOP
    assert bus.state is CanLinkState.STOPPING
    generated_stop = transport.sent[-1]
    assert bus.pending_command_id == int.from_bytes(generated_stop.data[1:3], "big")

    transport.incoming.append(stop_ack(int.from_bytes(generated_stop.data[1:3], "big")))
    result = bus.service_once()
    assert result is not None and result.confirmed
    assert bus.state is CanLinkState.SAFE_STOPPED


# 意图：显式 STOP 抢占排队中的普通命令。
# 断言：两条命令排队后 STOP 使队列只余 STOP；只发送 STOP 帧；诊断含 stop_preempted。
def test_explicit_stop_preempts_queued_ordinary_commands() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)

    assert bus.send(command(1)).accepted
    assert bus.send(command(2)).accepted
    assert bus.send(stop(0x8002)).accepted
    assert bus.queued_count == 1
    assert bus.service_once() is None
    assert [frame.arbitration_id for frame in transport.sent] == [MCU_CAN_ID_STOP]
    assert any(item.code.value == "stop_preempted" for item in bus.diagnostics())


# 意图：STOP 被拒绝或超时都失败关闭链路。
# 断言：STOP_ACK 拒绝 -> LINK_LOST 且不确认；第二个实例 STOP 超时 -> LINK_LOST、
#       普通命令被 LINK_UNAVAILABLE 拒绝。
def test_stop_rejection_and_timeout_fail_closed() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    config = CanTransportConfig(stop_timeout_s=1.0, stop_retry_budget=0)
    bus = running_bus(transport, clock, config)

    assert bus.send(stop(0x8003)).accepted
    assert bus.service_once() is None
    transport.incoming.append(stop_ack(0x8003, accepted=False))
    rejected = bus.service_once()
    assert rejected is not None and not rejected.confirmed
    assert bus.state is CanLinkState.LINK_LOST

    second_transport = FakeTransport()
    second_clock = FakeClock()
    second_bus = running_bus(second_transport, second_clock, config)
    assert second_bus.send(stop(0x8004)).accepted
    assert second_bus.service_once() is None
    second_clock.advance(1.0)
    assert second_bus.service_once() is None
    assert second_bus.state is CanLinkState.LINK_LOST
    assert second_bus.send(command(4)).status is CanSendStatus.LINK_UNAVAILABLE


# 意图：STOP 重试保持 STOP 关联（不回退为普通命令）。
# 断言：超时后发送 retry_count=1 的 STOP；同 ID 重试的 STOP_ACK 确认且 SAFE_STOPPED。
def test_stop_retry_retains_stop_correlation() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    config = CanTransportConfig(stop_timeout_s=1.0, stop_retry_budget=1)
    bus = running_bus(transport, clock, config)

    assert bus.send(stop(0x8005)).accepted
    assert bus.service_once() is None
    clock.advance(1.0)
    assert bus.service_once() is None
    assert transport.sent[-1] == stop(0x8005, retry_count=1)

    transport.incoming.append(stop_ack(0x8005, retry_count=1))
    result = bus.service_once()
    assert result is not None and result.confirmed
    assert bus.state is CanLinkState.SAFE_STOPPED


# 意图：第 1 条 STOP 在途时第 2 条 STOP 被拒绝（单在途窗口）。
# 断言：第 2 条为 CORRELATION_CONFLICT；总线上只出现 1 条 STOP。
def test_second_stop_is_rejected_while_first_stop_is_in_flight() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)

    assert bus.send(stop(0x8006)).accepted
    assert bus.send(stop(0x8007)).status is CanSendStatus.CORRELATION_CONFLICT
    assert bus.service_once() is None
    assert [frame.arbitration_id for frame in transport.sent] == [MCU_CAN_ID_STOP]


# 意图：普通命令超时后的迟到 ACK 不确认、不改变在途 STOP。
# 断言：LATE 且不确认；pending 仍是升级 STOP 的 ID；记录不可暴露；外部队列为空。
def test_late_ack_does_not_confirm_after_command_timeout() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    config = CanTransportConfig(ack_timeout_s=1.0, ack_retry_budget=0, stop_timeout_s=1.0)
    bus = running_bus(transport, clock, config)

    assert bus.send(command(9)).accepted
    assert bus.service_once() is None
    clock.advance(1.0)
    assert bus.service_once() is None
    generated_stop = transport.sent[-1]
    transport.incoming.append(ack(9))
    late = bus.service_once()
    assert late is not None and late.status is CanReceiveStatus.LATE
    assert not late.confirmed
    assert bus.pending_command_id == int.from_bytes(generated_stop.data[1:3], "big")
    assert late.external_record is not None
    assert not late.external_record.exposure_allowed
    assert bus.external_records() == ()


# 意图：无关联 ACK 可观测但不可暴露、不可确认。
# 断言：UNCORRELATED、不确认；记录存在但不可暴露；外部队列为空。
def test_uncorrelated_ack_is_observable_but_not_externally_exposed() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock())
    transport.incoming.append(ack(0x1234))

    result = bus.service_once()

    assert result is not None and result.status is CanReceiveStatus.UNCORRELATED
    assert not result.confirmed
    assert result.external_record is not None
    assert not result.external_record.exposure_allowed
    assert bus.external_records() == ()


# 意图：bus-off 清空挂起工作，恢复后绝不回放过期流量。
# 断言：send 失败 -> BUS_OFF、无在途 ID；recover 后 ACTIVE、队列为空；新命令可发。
def test_bus_off_clears_pending_work_and_recovery_does_not_replay_it() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    assert bus.send(command(11)).accepted
    transport.send_error = CanBusOffError("bus-off")

    assert bus.service_once() is None
    assert bus.state is CanLinkState.BUS_OFF
    assert bus.pending_command_id is None
    assert bus.recover()
    assert bus.state is CanLinkState.ACTIVE
    assert bus.queued_count == 0
    assert bus.send(command(12)).accepted


# 意图：接收链路丢失即失败关闭，显式恢复后重新可用。
# 断言：LINK_LOST 时 send 被拒；recover 后新命令可入队。
def test_receive_link_loss_is_fail_closed_until_recovery() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    transport.receive_error = CanLinkLostError("link lost")

    assert bus.service_once() is None
    assert bus.state is CanLinkState.LINK_LOST
    assert bus.send(command(13)).status is CanSendStatus.LINK_UNAVAILABLE
    assert bus.recover()
    assert bus.send(command(14)).accepted


# 意图：订阅者快照在派发中可变，回调异常被隔离而不中断其他回调。
# 断言：第 1 次派发仅回调原始快照（first、third），callback_errors=1；
#       第 2 次派发包含中途新增的 second（快照隔离 + 退订生效）。
def test_subscriber_snapshot_is_mutable_and_callback_failures_are_isolated() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    calls: list[str] = []

    def second(_frame: object) -> None:
        calls.append("second")

    def first(_frame: object) -> None:
        calls.append("first")
        bus.unsubscribe(MCU_CAN_ID_ACK, first)
        bus.subscribe(MCU_CAN_ID_ACK, second)
        raise RuntimeError("subscriber failure")

    def third(_frame: object) -> None:
        calls.append("third")

    assert bus.subscribe(MCU_CAN_ID_ACK, first)
    assert bus.subscribe(MCU_CAN_ID_ACK, third)
    assert bus.send(command(15)).accepted
    assert bus.service_once() is None
    transport.incoming.append(ack(15))
    result = bus.service_once()
    assert result is not None
    assert result.callback_errors == 1
    assert calls == ["first", "third"]

    assert bus.send(command(16)).accepted
    assert bus.service_once() is None
    transport.incoming.append(ack(16))
    bus.service_once()
    assert calls[-3:] == ["third", "third", "second"]


# 意图：遥测交付给订阅者且从不确认命令。
# 断言：ACCEPTED、confirmed=False；信封来源/接口/序号正确；订阅者收到 TELEMETRY。
def test_telemetry_is_delivered_without_confirming_command() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    received: list[CanFrameKind] = []
    bus.subscribe(MCU_CAN_ID_TELEMETRY, lambda frame: received.append(frame.kind))

    transport.incoming.append(telemetry(17))
    result = bus.service_once()
    assert result is not None
    assert result.status is CanReceiveStatus.ACCEPTED
    assert not result.confirmed
    assert isinstance(result.envelope, CanTransportEnvelope)
    assert result.envelope.source == "mcu-can"
    assert result.envelope.interface == "injected-can"
    assert result.envelope.sequence == 0
    assert received == [CanFrameKind.TELEMETRY]


# 意图：遥测序号按 32 位半区间规则处理回绕，并拒绝重复与过期快照。
# 断言：0xFFFFFFFF 后接受 0（回绕前进）；重复 0 为 DUPLICATE；再来 0xFFFFFFFF 为 LATE。
def test_telemetry_ordering_handles_wrap_and_rejects_duplicate_or_stale_frames() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)

    # 数据表：(序号, 期望状态)，覆盖回绕/重复/过期四种情形。
    for sequence_no, status in (
        (0xFFFFFFFF, CanReceiveStatus.ACCEPTED),
        (0, CanReceiveStatus.ACCEPTED),
        (0, CanReceiveStatus.DUPLICATE),
        (0xFFFFFFFF, CanReceiveStatus.LATE),
    ):
        transport.incoming.append(telemetry(sequence_no))
        result = bus.service_once()
        assert result is not None and result.status is status


# 意图：故障遥测失败关闭链路，显式恢复后重新接受遥测。
# 断言：fault=link_lost 的遥测被接受但链路 LINK_LOST、在途清空、send 被拒；
#       recover 后接受新遥测且入口序号推进为 1。
def test_fault_telemetry_fails_closed_until_explicit_recovery() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    assert bus.send(command(19)).accepted
    assert bus.service_once() is None

    transport.incoming.append(telemetry(1, fault_code=4, device_mode=4))
    result = bus.service_once()
    assert result is not None and result.status is CanReceiveStatus.ACCEPTED
    assert not result.confirmed
    assert bus.state is CanLinkState.LINK_LOST
    assert bus.pending_command_id is None
    assert bus.send(command(20)).status is CanSendStatus.LINK_UNAVAILABLE

    assert bus.recover()
    transport.incoming.append(telemetry(0))
    recovered = bus.service_once()
    assert recovered is not None and recovered.status is CanReceiveStatus.ACCEPTED
    assert recovered.envelope is not None
    assert recovered.envelope.sequence == 1


# 意图：健康平面容量独立于命令平面受限。
# 断言：两次 INVALID_FRAME 后诊断只保留 1 条（最旧淘汰）、健康丢弃计数 1、
#       命令平面深度为 0。
def test_health_plane_capacity_is_bounded_independently_from_command_plane() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    config = CanTransportConfig(queue_capacity=2, diagnostic_capacity=1, health_capacity=1)
    bus = running_bus(transport, clock, config)

    assert bus.send(telemetry()).status is CanSendStatus.INVALID_FRAME
    assert bus.send(telemetry(2)).status is CanSendStatus.INVALID_FRAME
    assert len(bus.diagnostics()) == 1
    assert bus.runtime.health_drop_count == 1
    assert bus.runtime.command_depth == 0


# 意图：未排空的遥测平面按最旧淘汰并计数。
# 断言：容量 1 下发布两条后丢弃计数 1，取出的恰是较新的一条。
def test_runtime_telemetry_plane_drops_oldest_when_not_drained() -> None:
    transport = FakeTransport()
    bus = SafeCANBus(
        transport,
        clock=FakeClock(),
        config=CanTransportConfig(telemetry_capacity=1),
    )
    first = decode_can_frame(telemetry(1))
    second = decode_can_frame(telemetry(2))

    bus.runtime.publish_telemetry(first)
    bus.runtime.publish_telemetry(second)

    assert bus.runtime.telemetry_drop_count == 1
    assert bus.runtime.take_telemetry() == second


# 意图：墙上时钟回拨是失败即拒绝入口错误。
# 断言：第 1 条接受；回拨后 INVALID_FRAME、链路 LINK_LOST、诊断含 clock_rollback、
#       send 被 LINK_UNAVAILABLE 拒绝。
def test_wall_clock_rollback_is_an_observable_fail_closed_ingress_error() -> None:
    transport = FakeTransport()
    wall_times = iter((10.0, 9.0))
    bus = SafeCANBus(transport, clock=FakeClock(), wall_clock=lambda: next(wall_times))
    assert bus.start(background=False)

    transport.incoming.append(telemetry(1))
    first = bus.service_once()
    assert first is not None and first.status is CanReceiveStatus.ACCEPTED

    transport.incoming.append(telemetry(2))
    second = bus.service_once()
    assert second is not None and second.status is CanReceiveStatus.INVALID_FRAME
    assert bus.state is CanLinkState.LINK_LOST
    assert any(item.code.value == "clock_rollback" for item in bus.diagnostics())
    assert bus.send(command(24)).status is CanSendStatus.LINK_UNAVAILABLE


# 意图：帧携带的单调观测回拨被拒绝且不暴露。
# 断言：第 2 条 INVALID_FRAME、原因含 moved backwards、不可暴露；外部投影只剩
#       序号 1；链路 LINK_LOST；诊断含 CLOCK_ROLLBACK。
def test_monotonic_rollback_is_rejected_without_external_exposure() -> None:
    transport = FakeTransport()
    bus = running_bus(transport, FakeClock())
    first_frame = telemetry(1)
    second_frame = telemetry(2)
    # 帧携带的主机单调观测 10.0 -> 9.0：倒退即失败。
    transport.incoming.extend(
        (
            CanFrame(
                first_frame.arbitration_id,
                first_frame.data,
                observed_monotonic_ts=10.0,
                observed_wall_ts=20.0,
            ),
            CanFrame(
                second_frame.arbitration_id,
                second_frame.data,
                observed_monotonic_ts=9.0,
                observed_wall_ts=21.0,
            ),
        )
    )

    first = bus.service_once()
    second = bus.service_once()

    assert first is not None and first.status is CanReceiveStatus.ACCEPTED
    assert second is not None and second.status is CanReceiveStatus.INVALID_FRAME
    assert second.external_record is not None
    assert second.external_record.reason == "monotonic observation moved backwards"
    assert not second.external_record.exposure_allowed
    assert tuple(record.sequence_no for record in bus.external_records()) == (1,)
    assert bus.state is CanLinkState.LINK_LOST
    assert any(item.code is CanDiagnosticCode.CLOCK_ROLLBACK for item in bus.diagnostics())


# 意图：被拒绝的普通 ACK 不算确认，并失败关闭链路。
# 断言：ACCEPTED 但 confirmed=False；链路 LINK_LOST；后续 send 被拒。
def test_rejected_ordinary_ack_is_not_confirmation_and_fails_closed() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    assert bus.send(command(21)).accepted
    assert bus.service_once() is None

    transport.incoming.append(rejected_ack(21))
    result = bus.service_once()
    assert result is not None and result.status is CanReceiveStatus.ACCEPTED
    assert not result.confirmed
    assert bus.state is CanLinkState.LINK_LOST
    assert bus.send(command(22)).status is CanSendStatus.LINK_UNAVAILABLE


# 意图：STOP 可抢占正阻塞在传输 send 中的普通命令。
# 断言：命令 send 阻塞期间 STOP 入队；释放后命令已发送、STOP 在队列头部；
#       下一轮轮询发送 STOP，pending 为 STOP 的 ID。
def test_stop_preempts_a_command_blocked_inside_transport_send() -> None:
    class BlockingSendTransport(FakeTransport):
        def __init__(self) -> None:
            super().__init__()
            self.send_entered = threading.Event()
            self.release_send = threading.Event()

        def send(self, frame: CanFrame) -> None:
            if frame.arbitration_id == MCU_CAN_ID_COMMAND:
                self.send_entered.set()
                assert self.release_send.wait(1.0)
            super().send(frame)

    transport = BlockingSendTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    assert bus.send(command(23)).accepted
    service_thread = threading.Thread(target=bus.service_once)
    service_thread.start()
    assert transport.send_entered.wait(1.0)

    assert bus.send(stop(0x8017)).accepted
    transport.release_send.set()
    service_thread.join(1.0)
    assert not service_thread.is_alive()
    assert bus.queued_count == 1

    assert bus.service_once() is None
    assert bus.pending_command_id == 0x8017
    assert [frame.arbitration_id for frame in transport.sent] == [MCU_CAN_ID_COMMAND, MCU_CAN_ID_STOP]


# 意图：生命周期操作（configure）阻塞期间的 shutdown 保持 False，直到清理完成。
# 断言：configure 阻塞时 shutdown(0) 返回 False 且状态 DEACTIVATING、cleanup 未开始；
#       释放后仍 False；cleanup 释放后 start 线程以 False 结束、事件序列
#       configure -> deactivate -> cleanup、终态 CLEANED、shutdown 可重试成功。
def test_shutdown_during_lifecycle_operation_stays_false_until_cleanup_finishes() -> None:
    class BlockingConfigureAdapter(RecordingAdapter):
        def __init__(self) -> None:
            super().__init__()
            self.configure_entered = threading.Event()
            self.release_configure = threading.Event()
            self.cleanup_entered = threading.Event()
            self.release_cleanup = threading.Event()

        def configure(self) -> bool:
            self.events.append("configure")
            self.configure_entered.set()
            assert self.release_configure.wait(1.0)
            return True

        def cleanup(self) -> bool:
            self.events.append("cleanup")
            self.cleanup_entered.set()
            assert self.release_cleanup.wait(1.0)
            return True

    adapter = BlockingConfigureAdapter()
    runtime = DeviceRuntime(
        adapter,
        command_capacity=1,
        telemetry_capacity=1,
        health_capacity=1,
        max_subscribers_per_id=1,
        poll_interval_s=0.001,
    )
    start_result: list[bool] = []
    start_thread = threading.Thread(target=lambda: start_result.append(runtime.start(background=False)))
    start_thread.start()
    assert adapter.configure_entered.wait(1.0)

    assert not runtime.shutdown(timeout_s=0.0)
    assert runtime.state is DeviceRuntimeState.DEACTIVATING
    assert not adapter.cleanup_entered.is_set()

    adapter.release_configure.set()
    assert adapter.cleanup_entered.wait(1.0)
    assert not runtime.shutdown(timeout_s=0.0)
    assert runtime.state is DeviceRuntimeState.DEACTIVATING

    adapter.release_cleanup.set()
    start_thread.join(1.0)
    assert not start_thread.is_alive()
    assert start_result == [False]
    assert adapter.events == ["configure", "deactivate", "cleanup"]
    assert runtime.state is DeviceRuntimeState.CLEANED
    assert runtime.shutdown(timeout_s=0.0)


# 意图：open 阻塞期间并发 shutdown 使启动失效，端口随后被关闭。
# 断言：open 阻塞时 shutdown(0) 返回 False、端口未关闭、状态 DEACTIVATING；
#       释放后 start 以 False 结束、端口已关闭、链路 SHUTDOWN、shutdown 可重试成功。
def test_start_cannot_reactivate_after_concurrent_shutdown() -> None:
    class BlockingOpenTransport(FakeTransport):
        def __init__(self) -> None:
            super().__init__()
            self.open_entered = threading.Event()
            self.release_open = threading.Event()

        def open(self) -> None:
            self.open_entered.set()
            assert self.release_open.wait(1.0)
            super().open()

    transport = BlockingOpenTransport()
    bus = SafeCANBus(transport, clock=FakeClock())
    start_result: list[bool] = []
    start_thread = threading.Thread(target=lambda: start_result.append(bus.start(background=False)))
    start_thread.start()
    assert transport.open_entered.wait(1.0)

    assert not bus.shutdown(timeout_s=0.0)
    assert not transport.closed
    assert bus.runtime.state is DeviceRuntimeState.DEACTIVATING
    transport.release_open.set()
    start_thread.join(1.0)
    assert start_result == [False]
    assert transport.closed
    assert bus.state is CanLinkState.SHUTDOWN
    assert bus.shutdown(timeout_s=0.0)


# 意图：初次 join 超时后 shutdown 可重试，最终关闭端口。
# 断言：receive 阻塞期间两次 shutdown(0) 均 False；释放 receive 后 shutdown(1.0)
#       成功且端口关闭。
def test_shutdown_retries_join_after_an_initial_timeout() -> None:
    class BlockingReceiveTransport(FakeTransport):
        def __init__(self) -> None:
            super().__init__()
            self.receive_entered = threading.Event()
            self.release_receive = threading.Event()

        def receive(self, timeout_s: float) -> CanFrame | None:
            del timeout_s
            self.receive_entered.set()
            assert self.release_receive.wait(1.0)
            return None

    transport = BlockingReceiveTransport()
    bus = SafeCANBus(transport, clock=FakeClock())
    assert bus.start(background=True)
    assert transport.receive_entered.wait(1.0)

    assert not bus.shutdown(timeout_s=0.0)
    assert not bus.shutdown(timeout_s=0.0)
    transport.release_receive.set()
    assert bus.shutdown(timeout_s=1.0)
    assert transport.closed


# 意图：与关闭竞争的接收返回的帧不被分发或发布。
# 断言：阻塞 receive 返回遥测后 shutdown 成功；外部投影为空、外部深度 0。
def test_shutdown_does_not_publish_a_frame_returned_by_an_inflight_receive() -> None:
    class BlockingFrameReceiveTransport(FakeTransport):
        def __init__(self) -> None:
            super().__init__()
            self.receive_entered = threading.Event()
            self.release_receive = threading.Event()

        def receive(self, timeout_s: float) -> CanFrame | None:
            del timeout_s
            self.receive_entered.set()
            assert self.release_receive.wait(1.0)
            return telemetry(91)

    transport = BlockingFrameReceiveTransport()
    bus = SafeCANBus(transport, clock=FakeClock())
    assert bus.start(background=True)
    assert transport.receive_entered.wait(1.0)

    assert not bus.shutdown(timeout_s=0.0)
    transport.release_receive.set()
    assert bus.shutdown(timeout_s=1.0)
    assert bus.external_records() == ()
    assert bus.external_depth == 0


# 意图：恢复操作无法在并发 shutdown 后重新激活链路。
# 断言：recover 阻塞期间 shutdown(0) 返回 False、端口未关闭、状态 DEACTIVATING；
#       释放后 recover 返回 False、端口关闭、链路 SHUTDOWN、shutdown 可重试成功。
def test_recovery_cannot_reactivate_after_concurrent_shutdown() -> None:
    class BlockingRecoverTransport(FakeTransport):
        def __init__(self) -> None:
            super().__init__()
            self.recover_entered = threading.Event()
            self.release_recover = threading.Event()

        def recover(self) -> bool:
            self.recover_entered.set()
            assert self.release_recover.wait(1.0)
            return True

    transport = BlockingRecoverTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock)
    transport.receive_error = CanLinkLostError("link lost")
    assert bus.service_once() is None

    recovery_result: list[bool] = []
    recovery_thread = threading.Thread(target=lambda: recovery_result.append(bus.recover()))
    recovery_thread.start()
    assert transport.recover_entered.wait(1.0)
    assert not bus.shutdown(timeout_s=0.0)
    assert not transport.closed
    assert bus.runtime.state is DeviceRuntimeState.DEACTIVATING
    transport.release_recover.set()
    recovery_thread.join(1.0)

    assert recovery_result == [False]
    assert transport.closed
    assert bus.state is CanLinkState.SHUTDOWN
    assert bus.shutdown(timeout_s=0.0)


# 意图：shutdown 停止 worker、关闭端口并拒绝后续 send。
# 断言：后台启动后 shutdown 成功；端口关闭；链路 SHUTDOWN；send 为 NOT_RUNNING。
def test_shutdown_stops_worker_closes_transport_and_rejects_future_send() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = SafeCANBus(transport, clock=clock)
    assert bus.start(background=True)
    assert bus.shutdown(timeout_s=1.0)
    assert transport.closed
    assert bus.state is CanLinkState.SHUTDOWN
    assert bus.send(command(18)).status is CanSendStatus.NOT_RUNNING


# 意图：诊断存储有界但错误计数累计全部失败（含被淘汰的记录）。
# 断言：容量 2 下 3 次失败只保留 2 条诊断；get_error_count 至少覆盖全部失败。
def test_error_count_is_bounded_by_diagnostic_storage_but_counts_all_failures() -> None:
    transport = FakeTransport()
    clock = FakeClock()
    bus = running_bus(transport, clock, CanTransportConfig(diagnostic_capacity=2))
    assert bus.send(command(1)).status is CanSendStatus.QUEUED
    assert bus.send(command(1)).status is CanSendStatus.CORRELATION_CONFLICT
    assert bus.send(telemetry()).status is CanSendStatus.INVALID_FRAME
    assert len(bus.diagnostics()) == 2
    assert bus.get_error_count() >= 1
