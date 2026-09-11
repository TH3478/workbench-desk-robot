"""wbcan 虚拟 SocketCAN 入口探针（kernel/wbcan/test_socketcan_ingress.py）的报告校验测试。

探针在特权环境才能实际驱动 vcan/wbcan 设备，因此这些测试只验证报告结构与
校验逻辑：NOT_EXECUTED 报告、FAIL 报告（不能通过 --require-pass）、内核配置
摘要格式，以及 CLI 能物化合法的兜底报告。物理 CAN/MCU/执行器/硬实时断言必须
保持 NOT_EXECUTED（host-can-transport-v1.md 证据限制）。
"""

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "kernel" / "wbcan" / "test_socketcan_ingress.py"
# 以 socketcan_ingress_probe 名称按文件路径加载探针模块（非包，无 __init__）。
SPEC = importlib.util.spec_from_file_location("socketcan_ingress_probe", MODULE_PATH)
assert SPEC and SPEC.loader
probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = probe
SPEC.loader.exec_module(probe)


# 意图：NOT_EXECUTED 兜底报告结构合法，物理声明保持未执行。
# 断言：validate_report 通过；result 与 physical_can/mcu/actuator/hard_real_time
#       全部为 NOT_EXECUTED。
def test_not_executed_report_is_valid_and_keeps_physical_claims_unexecuted() -> None:
    report = probe._not_executed_report("wbcan0", "virtual-wbcan", "root is unavailable")

    probe.validate_report(report)

    assert report["result"] == "NOT_EXECUTED"
    assert report["physical_can"] == "NOT_EXECUTED"
    assert report["mcu"] == "NOT_EXECUTED"
    assert report["actuator"] == "NOT_EXECUTED"
    assert report["hard_real_time"] == "NOT_EXECUTED"


# 意图：带部分证据的 FAIL 报告结构合法，但不能满足 require_pass。
# 断言：validate_report 通过；require_pass=True 时抛含 required PASS 的 ValueError。
def test_failed_report_with_partial_evidence_is_valid_but_cannot_satisfy_require_pass() -> None:
    report = probe._base_report("wbcan0", "virtual-wbcan")
    report.update(
        {
            "result": "FAIL",
            "error": "peer did not answer",
            "checks": [{"name": "peer response", "result": "FAIL", "detail": "timeout"}],
            "records": {},
            "cleanup": {
                "socket_open": None,
                "peer_closed": None,
                "worker_alive": None,
                "external_depth": None,
            },
        }
    )

    probe.validate_report(report)

    with pytest.raises(ValueError, match="required PASS"):
        probe.validate_report(report, require_pass=True)


# 意图：PASS 校验要求小写的十六进制内核配置摘要。
# 断言：kernel_config_sha256 为非摘要字符串时 validate_report 抛
#       含 kernel_config_sha256 的 ValueError。
def test_pass_validation_requires_a_lowercase_kernel_config_digest() -> None:
    report = probe._base_report("wbcan0", "virtual-wbcan")
    report.update(
        {
            "result": "FAIL",
            "error": "synthetic failure",
            "checks": [{"name": "probe", "result": "FAIL", "detail": "synthetic"}],
            "records": {},
            "cleanup": {
                "socket_open": None,
                "peer_closed": None,
                "worker_alive": None,
                "external_depth": None,
            },
            "kernel_config_sha256": "not-a-digest",
        }
    )

    with pytest.raises(ValueError, match="kernel_config_sha256"):
        probe.validate_report(report)


# 意图：CLI 能物化合法的 NOT_EXECUTED 兜底报告（子进程端到端）。
# 断言：退出码 0；写出的 JSON 通过 validate_report 且 result 为 NOT_EXECUTED。
def test_cli_can_materialize_a_valid_fallback_report(tmp_path: Path) -> None:
    report_path = tmp_path / "fallback.json"
    result = subprocess.run(
        [
            sys.executable,
            str(MODULE_PATH),
            "wbcan0",
            "--source",
            "virtual-wbcan",
            "--write-not-executed-report",
            str(report_path),
            "--not-executed-reason",
            "module setup failed",
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(report_path.read_text(encoding="utf-8"))
    probe.validate_report(payload)
    assert payload["result"] == "NOT_EXECUTED"
