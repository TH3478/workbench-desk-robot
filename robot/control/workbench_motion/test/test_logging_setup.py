"""统一日志配置的单元测试。

证明阶段 0 验收标准：统一格式携带 run_id，且配置幂等（无堆叠 handler、不依赖
``print``）。
"""

from __future__ import annotations

import io
import logging

from workbench_motion.logging_setup import (
    LOG_FORMAT,
    _ContextDefaultsFilter,
    configure_logging,
    get_action_logger,
)


def _capture_handler(logger: logging.Logger) -> io.StringIO:
    """挂一个镜像统一格式 + 上下文过滤器的 StringIO handler。

    这里不能依赖 capsys/capfd：真正的 StreamHandler 在构造时绑定 sys.stderr——早于
    pytest 换流——所以 capsys 永远看不到它。本 handler 在确定性捕获的同时，走包安装
    的同一 LOG_FORMAT 与默认值过滤器。
    """
    stream = io.StringIO()
    handler = logging.StreamHandler(stream)
    handler.setFormatter(logging.Formatter(LOG_FORMAT))
    handler.addFilter(_ContextDefaultsFilter())
    logger.addHandler(handler)
    return stream


def test_configure_is_idempotent() -> None:
    logger = logging.getLogger("workbench_motion")
    configure_logging()
    count_after_first = len(logger.handlers)
    configure_logging()
    count_after_second = len(logger.handlers)
    assert count_after_first == count_after_second


def test_package_logger_does_not_propagate_to_root() -> None:
    configure_logging()
    assert logging.getLogger("workbench_motion").propagate is False


def test_format_carries_run_and_action_ids() -> None:
    configure_logging()
    logger = logging.getLogger("workbench_motion.fmt")
    stream = _capture_handler(logger)
    log = get_action_logger("workbench_motion.fmt", run_id="run-xyz", action_id="act-7")
    log.info("action started")
    line = stream.getvalue()
    assert "run=run-xyz" in line
    assert "action=act-7" in line
    assert "action started" in line


def test_plain_log_without_context_uses_defaults() -> None:
    configure_logging()
    logger = logging.getLogger("workbench_motion.plain")
    stream = _capture_handler(logger)
    logger.warning("no context here")
    line = stream.getvalue()
    # 上下文过滤器必须提供默认值，使格式化器永不崩溃。
    assert "run=-" in line
    assert "action=-" in line
