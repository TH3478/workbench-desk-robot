"""Motion 包的统一日志配置。

规则（见 robot/control/PLAN.md「全局约定」）：
- 每个模块使用 ``logging.getLogger(__name__)``；绝不使用 ``print``。
- 级别：DEBUG 规划细节 / INFO 动作起止 / WARNING 重试降级 / ERROR 失败与拒绝。
- 每条动作日志行携带 ``run_id`` + ``action_id`` 供人工回溯。

日志面向人类与调试，刻意不作为证据通道：凡被 ``evidence_refs`` 引用的对象必须有
稳定 id，并经由 :mod:`workbench_motion.evidence`，而非日志行。
"""

from __future__ import annotations

import logging

# 我们总是希望记录上存在这些字段，以便格式化器在遇到未提供动作上下文的普通
# ``logger.info(...)`` 调用时不会崩溃。
_DEFAULT_CONTEXT = {"run_id": "-", "action_id": "-"}

LOG_FORMAT = "%(asctime)s %(levelname)s [run=%(run_id)s action=%(action_id)s] %(name)s: %(message)s"


class _ContextDefaultsFilter(logging.Filter):
    """为缺失 ``run_id``/``action_id`` 的记录补默认值。"""

    def filter(self, record: logging.LogRecord) -> bool:
        for key, value in _DEFAULT_CONTEXT.items():
            if not hasattr(record, key):
                setattr(record, key, value)
        return True


def configure_logging(level: int = logging.INFO) -> None:
    """在 ``workbench_motion`` 日志器上安装统一的 handler + formatter。

    幂等：多次调用不会堆叠 handler。挂到包日志器（而非根日志器）上，让宿主应用保持
    对全局日志配置的控制。
    """
    logger = logging.getLogger("workbench_motion")
    logger.setLevel(level)
    logger.propagate = False

    if any(getattr(h, "_workbench_motion", False) for h in logger.handlers):
        return

    handler = logging.StreamHandler()
    handler.setFormatter(logging.Formatter(LOG_FORMAT))
    handler.addFilter(_ContextDefaultsFilter())
    handler._workbench_motion = True  # type: ignore[attr-defined]  # 幂等性标记
    logger.addHandler(handler)


def get_action_logger(name: str, *, run_id: str, action_id: str = "-") -> logging.LoggerAdapter:
    """返回在每行日志上盖 ``run_id``/``action_id`` 戳的日志器适配器。

    每个动作一个，使该动作的所有日志行携带相同 id。
    """
    return logging.LoggerAdapter(logging.getLogger(name), {"run_id": run_id, "action_id": action_id})
