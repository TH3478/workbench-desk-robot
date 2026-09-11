"""虚拟 MCU 的确定性安全状态机（CAN 命令入口）。

本模块是 firmware/virtual_mcu/ 退役安全参考的剩余核心：状态、命令
接受/拒绝规则与看门狗超时路径的确定性模型，供早期 Python 消费者与
测试保持兼容（firmware/mcu/README.md「权威边界」）。它不是 C 协议、
固件或物理安全行为的证据。

命令入口是 VirtualMcu.command()：CAN 0x100 command / 0x080 stop 帧
携带的 opcode 语义经上层解码为命令词后，以字符串形式进入本入口；
状态机只按命令词与当前状态决定接受或拒绝，返回结果对象，绝不直接
触碰 CAN 载荷、仲裁标识符或线上字节。

与 C 实现（firmware/mcu/core/state_machine.h）的同名常量对照：
    McuState.IDLE / EXECUTING / SAFE_STOP / FAULT
        ↔ MCU_STATE_IDLE / MCU_STATE_EXECUTING / MCU_STATE_SAFE_STOP
          / MCU_STATE_FAULT；
    McuCommandStatus.ACCEPTED / REJECTED
        ↔ MCU_RESULT_ACCEPTED(0) / MCU_RESULT_REJECTED(1)，
          即线上 result_code；
    fault_code "WATCHDOG_TIMEOUT"
        ↔ MCU_FAULT_WATCHDOG_EXPIRED（名称不同，语义同为软件链路
          看门狗到期进入故障）。
"""

from dataclasses import dataclass
from enum import StrEnum


class McuState(StrEnum):
    """安全状态全集，与 C 侧 mcu_state_t（core/state_machine.h）同名对照。"""

    # IDLE：上电初态与复位目标态；execute 只能从本状态进入执行。
    IDLE = "idle"
    # EXECUTING：运动中或保持中（C 侧经设备模式 MOVING / HOLDING 细分）。
    EXECUTING = "executing"
    # SAFE_STOP：安全停止；stop 幂等保持，execute 被拒绝，离开经 reset。
    SAFE_STOP = "safe_stop"
    # FAULT：锁存故障；stop 仍被接受并迁移到 SAFE_STOP（fault_code
    # 保持锁存），只有 reset 清除故障码并回到 IDLE。
    FAULT = "fault"


class McuCommandStatus(StrEnum):
    """命令结果状态：与 C 侧 result_code / MCU_RESULT_*（0/1）对照。"""

    ACCEPTED = "accepted"
    REJECTED = "rejected"


class McuCommandRejection(StrEnum):
    """拒绝原因枚举：命令入口的失败即拒绝诊断，不改变状态或故障码。"""

    # 非字符串输入（如 None）；字符串命令之外的类型一律拒绝。
    NON_STRING = "non_string_command"
    # 空字符串：没有可解析的命令词。
    EMPTY = "empty_command"
    # 首尾含空白字符：按 MALFORMED 拒绝，不做隐式修剪。
    MALFORMED = "malformed_command"
    # 不在白名单中的命令词（如拼写错误）。
    UNKNOWN = "unknown_command"
    # 白名单内的命令词，但当前状态不允许该转移。
    INVALID_STATE = "invalid_state_transition"


@dataclass(frozen=True)
class McuCommandResult:
    """一次命令尝试的确定性结果：状态 + 转移后状态 + 可选拒绝原因。"""

    status: McuCommandStatus
    state: McuState
    reason: McuCommandRejection | None = None

    def __post_init__(self) -> None:
        # 接受与拒绝互斥：ACCEPTED 不得携带原因，REJECTED 必须携带原因。
        if self.status is McuCommandStatus.ACCEPTED and self.reason is not None:
            raise ValueError("accepted command results cannot have a rejection reason")
        if self.status is McuCommandStatus.REJECTED and self.reason is None:
            raise ValueError("rejected command results require a rejection reason")

    @property
    def accepted(self) -> bool:
        """快捷谓词：结果是否为 ACCEPTED。"""
        return self.status is McuCommandStatus.ACCEPTED

    @property
    def rejected(self) -> bool:
        """快捷谓词：结果是否为 REJECTED。"""
        return self.status is McuCommandStatus.REJECTED


# 命令词白名单与每个命令词允许的当前状态集合——接受/拒绝规则的唯一来源。
# "stop" 在任何状态都可接受：对应 C 侧 STOP（0x080）绕过会话闸门与
# 回放窗口、在普通命令之前被处理的安全语义（mcu-protocol-v1.md
# 「关联、重试与回绕语义」）；普通执行需要先回到 IDLE。
_COMMAND_ALLOWED_STATES: dict[str, frozenset[McuState]] = {
    "stop": frozenset(McuState),
    "reset": frozenset({McuState.SAFE_STOP, McuState.FAULT}),
    "execute": frozenset({McuState.IDLE}),
    "complete": frozenset({McuState.EXECUTING}),
}


class VirtualMcu:
    """P0 安全边界的小型确定性模型。"""

    def __init__(self) -> None:
        # 上电初态 IDLE、无故障码；与 C 侧 mcu_sm_init() 的上电语义一致：
        # 开启非执行会话，不恢复任何挂起运动。
        self.state = McuState.IDLE
        self.fault_code: str | None = None

    def command(self, command: object) -> McuCommandResult:
        """CAN 命令入口：按固定顺序校验并（可选）应用一次命令词。

        校验顺序：非字符串 → 空串 → 首尾空白 → 未知命令词 →
        状态不允许。任何拒绝都不改变 state 与 fault_code（失败即拒绝）；
        接受时按命令词执行转移，并返回转移后的状态。
        """
        if not isinstance(command, str):
            return self._reject(McuCommandRejection.NON_STRING)
        if not command:
            return self._reject(McuCommandRejection.EMPTY)
        if command != command.strip():
            return self._reject(McuCommandRejection.MALFORMED)
        # 白名单查询：未知命令词在状态校验之前拒绝。
        allowed_states = _COMMAND_ALLOWED_STATES.get(command)
        if allowed_states is None:
            return self._reject(McuCommandRejection.UNKNOWN)
        # 状态闸门：命令词合法但当前状态不允许该转移时拒绝。
        if self.state not in allowed_states:
            return self._reject(McuCommandRejection.INVALID_STATE)

        # 以下分支穷尽白名单的四个命令词，转移互斥。
        if command == "stop":
            # stop：任意状态 → SAFE_STOP；幂等，fault_code 保持锁存
            # （对应 C 侧 STOP 路径：只有成功 stop_ack 才确认已停止状态）。
            self.state = McuState.SAFE_STOP
        elif command == "reset":
            # reset：SAFE_STOP/FAULT → IDLE，并清除锁存的故障码；
            # 对应 C 侧可信复位闸门（原因清除 + 授权），协议 v1.0
            # 帧本身绝不构成复位授权。
            self.state = McuState.IDLE
            self.fault_code = None
        elif command == "execute":
            # execute：IDLE → EXECUTING（C 侧设备模式 MOVING 的执行阶段）。
            self.state = McuState.EXECUTING
        elif command == "complete":
            # complete：EXECUTING → IDLE（动作完成，不确认任何 CAN ACK）。
            self.state = McuState.IDLE
        return McuCommandResult(McuCommandStatus.ACCEPTED, self.state)

    def _reject(self, reason: McuCommandRejection) -> McuCommandResult:
        """构造拒绝结果：状态与故障码保持原样（失败即拒绝，无副作用）。"""
        return McuCommandResult(McuCommandStatus.REJECTED, self.state, reason)

    def watchdog_timeout(self) -> McuState:
        """软件链路看门狗到期：进入 FAULT 并锁存 WATCHDOG_TIMEOUT 故障码。

        对应 C 侧 MCU_EVENT_WATCHDOG_EXPIRED → MCU_STATE_FAULT +
        MCU_FAULT_WATCHDOG_EXPIRED（fault_code 名称不同、语义相同）；
        返回新状态供调用方观测。
        """
        self.state = McuState.FAULT
        self.fault_code = "WATCHDOG_TIMEOUT"
        return self.state
