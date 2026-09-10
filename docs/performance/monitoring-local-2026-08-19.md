# 监控采集器本机证据（2026-08-19）

状态：**已测量本机软件；目标硬件与物理来源 NOT_EXECUTED**。

本报告在 Linux x86_64、Python 3.12.3、Intel Core i7-14650HX（24 个逻辑 CPU）上生成。
它不是 Jetson Orin 或物理机器人发布证据。

```bash
python3 tools/scripts/benchmark_monitoring.py \
  --iterations 10000 \
  --output runs/performance/monitoring.json
```

| 测量项 | 本机结果 |
|---|---:|
| 每次快照 CPU 耗时 | 72,963 ns |
| 每次快照墙钟耗时 | 72,979 ns |
| 快照前后 RSS | 16,195,584 / 16,207,872 bytes |
| 快照后 PSS | 13,584,384 bytes |
| 10,000 次快照期间的调度器上下文切换 | 7 |
| 快照前后线程数 | 1 / 1 |
| 采集器自有的周期性唤醒 | 0 |
| 快照 JSON / Prometheus 投影 | 8,313 / 840 bytes |

生成的合成投影对完整的新鲜快照为 `healthy`，对缺失与陈旧的关键来源为 `unknown`，
对急停通道故障为 `fault`。原始报告保留在被忽略的 `runs/` 路径下，可用上述命令
重新生成。

这些数字只是开发主机的回归参考。目标级别的 Jetson CPU/RSS/唤醒/输出测量以及
标定后的急停、BMS、CAN、Nav2、Motion 与感知来源证据仍为 **NOT_EXECUTED**，不得
从这些固定装置推断。
