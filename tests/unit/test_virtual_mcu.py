# test_virtual_mcu.py —— 虚拟 MCU 状态机（firmware/virtual_mcu）的单元测试。
#
# 验证 CAN 命令入口 VirtualMcu.command() 的确定性行为：命令词白名单、
# 状态闸门、失败即拒绝（拒绝绝不改变 state / fault_code）与 stop 的
# 全状态幂等性，以及看门狗超时进入 FAULT 的路径。测试只断言状态机的
# 确定性逻辑，不涉及 CAN 载荷、线上字节或真实安全硬件。
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware/virtual_mcu"))

from workbench_virtual_mcu import (
    McuCommandRejection,
    McuCommandStatus,
    McuState,
    VirtualMcu,
)


class VirtualMcuTests(unittest.TestCase):
    """虚拟 MCU 状态机确定性行为测试。"""

    def test_stop_and_watchdog_enter_safe_states(self) -> None:
        """意图：主路径 execute → complete → execute → stop → reset →
        看门狗超时逐个迁移到期望状态。断言要点：每次命令返回的状态
        依次为 EXECUTING / IDLE / EXECUTING / SAFE_STOP / IDLE / FAULT，
        即 IDLE ↔ EXECUTING 双向往返与两类安全终点。"""
        mcu = VirtualMcu()
        self.assertEqual(mcu.command("execute").state, McuState.EXECUTING)
        self.assertEqual(mcu.command("complete").state, McuState.IDLE)
        self.assertEqual(mcu.command("execute").state, McuState.EXECUTING)
        self.assertEqual(mcu.command("stop").state, McuState.SAFE_STOP)
        self.assertEqual(mcu.command("reset").state, McuState.IDLE)
        self.assertEqual(mcu.watchdog_timeout(), McuState.FAULT)

    def test_unknown_empty_non_string_and_malformed_commands_are_rejected(self) -> None:
        """意图：四类形态非法的输入（未知词、空串、非字符串、带空白）
        全部失败即拒绝。断言要点：status == REJECTED、reason 与预期
        拒绝原因逐项相等、state 保持 IDLE 且 fault_code 保持 None——
        拒绝绝不产生状态或故障副作用。"""
        for command, reason in (
            ("typo", McuCommandRejection.UNKNOWN),
            ("", McuCommandRejection.EMPTY),
            (None, McuCommandRejection.NON_STRING),
            (" execute", McuCommandRejection.MALFORMED),
        ):
            with self.subTest(command=command):
                mcu = VirtualMcu()
                result = mcu.command(command)

                self.assertEqual(result.status, McuCommandStatus.REJECTED)
                self.assertTrue(result.rejected)
                self.assertFalse(result.accepted)
                self.assertEqual(result.reason, reason)
                self.assertEqual(result.state, McuState.IDLE)
                self.assertIsNone(mcu.fault_code)

    def test_rejected_state_transitions_do_not_mutate_state_or_fault(self) -> None:
        """意图：合法命令词在非法状态（白名单矩阵的 False 项）下被拒绝。
        断言要点：reason == INVALID_STATE，且 mcu.state 与 mcu.fault_code
        在命令前后逐项相等——状态闸门拒绝不产生任何副作用。"""
        cases = (
            (McuState.IDLE, None, "complete"),
            (McuState.EXECUTING, None, "execute"),
            (McuState.EXECUTING, None, "reset"),
            (McuState.SAFE_STOP, None, "execute"),
            (McuState.FAULT, "WATCHDOG_TIMEOUT", "execute"),
        )
        for state, fault_code, command in cases:
            with self.subTest(state=state, command=command):
                mcu = VirtualMcu()
                mcu.state = state
                mcu.fault_code = fault_code

                result = mcu.command(command)

                self.assertEqual(result.status, McuCommandStatus.REJECTED)
                self.assertEqual(result.reason, McuCommandRejection.INVALID_STATE)
                self.assertEqual(result.state, state)
                self.assertEqual(mcu.state, state)
                self.assertEqual(mcu.fault_code, fault_code)

    def test_reset_from_fault_is_the_only_fault_clear_path(self) -> None:
        """意图：看门狗超时进入 FAULT 后，reset 是回到 IDLE 并清除锁存
        故障码的唯一命令路径。断言要点：status == ACCEPTED、
        state == IDLE、fault_code 恢复为 None。"""
        mcu = VirtualMcu()
        mcu.watchdog_timeout()

        result = mcu.command("reset")

        self.assertEqual(result.status, McuCommandStatus.ACCEPTED)
        self.assertEqual(result.state, McuState.IDLE)
        self.assertIsNone(mcu.fault_code)

    def test_stop_is_accepted_from_every_state_and_is_idempotent(self) -> None:
        """意图：stop 在全部四个状态都被接受且幂等（重复 stop 仍接受）。
        断言要点：两次 stop 均为 ACCEPTED 且都落到 SAFE_STOP；FAULT 的
        锁存故障码在 stop 后保持 WATCHDOG_TIMEOUT（stop 不清除故障）。"""
        for state in McuState:
            with self.subTest(state=state):
                mcu = VirtualMcu()
                mcu.state = state
                mcu.fault_code = "WATCHDOG_TIMEOUT" if state is McuState.FAULT else None

                first = mcu.command("stop")
                second = mcu.command("stop")

                self.assertEqual(first.status, McuCommandStatus.ACCEPTED)
                self.assertEqual(second.status, McuCommandStatus.ACCEPTED)
                self.assertEqual(first.state, McuState.SAFE_STOP)
                self.assertEqual(second.state, McuState.SAFE_STOP)
                self.assertEqual(mcu.fault_code, "WATCHDOG_TIMEOUT" if state is McuState.FAULT else None)

    def test_command_matrix_has_one_explicit_outcome_per_state(self) -> None:
        """意图：以 4 x 4 显式矩阵核对每个（状态, 命令词）组合的接受性。
        断言要点：result.accepted 与矩阵期望逐项相等——每个组合只有
        一个确定性结果，不存在隐式默认分支。"""
        expected = {
            McuState.IDLE: {
                "stop": True,
                "reset": False,
                "execute": True,
                "complete": False,
            },
            McuState.EXECUTING: {
                "stop": True,
                "reset": False,
                "execute": False,
                "complete": True,
            },
            McuState.SAFE_STOP: {
                "stop": True,
                "reset": True,
                "execute": False,
                "complete": False,
            },
            McuState.FAULT: {
                "stop": True,
                "reset": True,
                "execute": False,
                "complete": False,
            },
        }
        for state, commands in expected.items():
            for command, accepted in commands.items():
                with self.subTest(state=state, command=command):
                    mcu = VirtualMcu()
                    mcu.state = state
                    mcu.fault_code = "WATCHDOG_TIMEOUT" if state is McuState.FAULT else None
                    result = mcu.command(command)

                    self.assertEqual(result.accepted, accepted)


if __name__ == "__main__":
    unittest.main()
