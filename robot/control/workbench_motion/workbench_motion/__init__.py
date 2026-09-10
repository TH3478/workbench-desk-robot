"""Motion（robot/control）语义动作适配器包。

阶段 0 只交付工程脚手架：统一日志配置与 EvidenceSink 接口加测试替身。尚无机械臂、
MoveIt 或抓取逻辑。
"""

from .evidence import EvidenceRef, EvidenceSink, ExecutionEvent, FakeEvidenceSink
from .logging_setup import configure_logging, get_action_logger

__all__ = [
    "EvidenceRef",
    "EvidenceSink",
    "ExecutionEvent",
    "FakeEvidenceSink",
    "configure_logging",
    "get_action_logger",
]
