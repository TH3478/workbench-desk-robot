"""供 Workbench 任务边界之间使用的共享工具。"""

from .file_lock import exclusive_file_lock

__all__ = ["exclusive_file_lock"]
