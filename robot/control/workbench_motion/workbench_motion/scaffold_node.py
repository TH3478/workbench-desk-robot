"""用于阶段 0 空世界自测的最小 Motion 节点。

它什么也不做，只是启动、记录一条携带 ``run_id`` 的统一启动日志行、向（假的）
EvidenceSink 追加一条启动 ExecutionEvent，然后要么运行一次受限自测并干净退出（默认），
要么无限 spin。它存在是为了在没有任何外部启动调试（无 Gazebo、无机械臂、无 MCU）的
情况下让包的 launch 可被演练。真正的适配器行为在后续阶段落地。

行为由 ``self_test_seconds`` ROS 参数控制：
- ``> 0``（默认 2.0）：spin 这么久，记录 "self-test passed"，以 0 退出。
  这是 CI 友好的路径——无需 SIGINT，launch 看到干净退出。
- ``<= 0``：无限 spin（交互使用；Ctrl-C 停止）。

``rclpy`` 是 apt/rosdep 管理的 ROS 2 运行时依赖，不是 uv 管理的。导入在 ``main``
内惰性进行，使本包的纯 Python 部分（evidence、logging_setup 及其测试）在没有 rclpy
的纯 uv 环境里也能导入并运行。
"""

from __future__ import annotations

import logging
import time
import uuid

from .evidence import ExecutionEvent, FakeEvidenceSink
from .logging_setup import configure_logging, get_action_logger


def main(args: list[str] | None = None) -> None:
    import rclpy
    from rclpy.executors import ExternalShutdownException
    from rclpy.node import Node

    configure_logging(logging.INFO)
    run_id = uuid.uuid4().hex[:12]

    class ScaffoldNode(Node):
        def __init__(self) -> None:
            super().__init__("workbench_motion_scaffold")
            self.log = get_action_logger("workbench_motion.scaffold", run_id=run_id)
            self.self_test_seconds = self.declare_parameter("self_test_seconds", 2.0).get_parameter_value().double_value
            # 阶段 0 的 sink 是测试替身；生产 sink 在后续阶段经 World Model 事件库
            # 适配器接入。
            self._sink = FakeEvidenceSink()
            ref = self._sink.append(ExecutionEvent(event_type="node_started", run_id=run_id, action_id="-"))
            self.log.info("workbench_motion scaffold node up; startup event ref=%s", ref)

    rclpy.init(args=args)
    node = ScaffoldNode()
    try:
        if node.self_test_seconds > 0:
            deadline = time.monotonic() + node.self_test_seconds
            while rclpy.ok() and time.monotonic() < deadline:
                rclpy.spin_once(node, timeout_sec=0.1)
            node.log.info("self-test passed; shutting down cleanly")
        else:
            rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == "__main__":
    main()
