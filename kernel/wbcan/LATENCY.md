# wbcan 虚拟延迟证据

延迟探针用 `monotonic_ns` 测量用户空间观测到的原生 SocketCAN 发送到接收时间。它只驱动虚拟 `wbcan` 网络设备。其报告不是物理 CAN、控制器 IRQ、收发器、MCU、执行器、PREEMPT_RT 或硬实时证据。

## 配置档

- `idle` 只运行受限的测量循环。
- `controlled-load` 在测量期间运行固定数量的 CPU 与 I/O worker。每个 CPU worker 反复哈希一块固定的 64 KiB 内存缓冲。每个 I/O worker 反复覆盖一个 4 KiB 临时文件并 `fsync` 它（在偏移零处写入），然后用 `fstat` 验证其受限大小。报告记录每个 worker 观测到的迭代次数、写入的逻辑字节数与观测到的最大文件大小。临时文件在每次运行后删除，因此该配置档的磁盘占用不随运行时长增长。每个 worker 在发布 `ready` 之前完成首次使用分配、文件设置与设置验证。父进程等待每个 worker，启动 monotonic 与进程 CPU 时钟，然后打开一个公共起始闸门。因此，设置位于受控负载测量窗口之外，`idle` 与 `controlled-load` 窗口可比较。
- `status-readers` 是独立的 Issue #155 比较配置档。它在使用同一延迟循环的同时反复读取受限的 debugfs 状态快照。

每个 worker 必须在两秒内就绪，并在受限的停机期限内停止。停机先请求协作停止，然后使用受限的 `terminate`/`kill` 兜底。父进程在关闭进程句柄前验证每个 worker 不再存活且有退出状态。就绪超时、协作停止超时、强制终止、非零退出、未验证的停机、短写、worker 异常、活动不完整、部分帧运行、重复、意外帧或时钟倒退产生 `FAIL` 证据。未产生实测活动的 worker 同样被拒绝。

## 重复战役

权威执行路径是特权 GitHub Actions `kernel-module` 任务。它构建并加载 `wbcan`，运行完整的驱动闸门，然后把重复的 idle/controlled-load 报告连同严格战役 JSON 一起记录并上传。在头文件匹配的 Linux 主机上，若有 root 权限、debugfs 与可用的 `wbcan0`，等效的本地命令是：

```bash
sudo make -C kernel/wbcan latency-campaign
```

默认战役运行三次 idle 重复与三次 controlled-load 重复，预热、采样、CAN ID、commit、内核、亲和性、时钟与环境字段完全一致。三次重复是这次托管比较的最低完整性预算；它们不是延迟验收阈值。受限上限是每次运行 20 次重复与 100,000 个实测采样。

如果本地环境无法构建/加载模块或访问 debugfs（例如 WSL 头文件与运行内核不匹配），把本地运行时战役保持 `NOT_EXECUTED`，并用托管 `kernel-module` 结果作为虚拟 wbcan 运行时证据。托管 PASS 不把声称扩展到该 runner 之外：物理 CAN、MCU、执行器、PREEMPT_RT 与硬实时验证保持 `NOT_EXECUTED`。

两个配置档报告保留每次运行的 P50/P95/P99/max、总体标准差抖动、可选期限错过、耗时、进程 CPU 时间、吞吐量、投递计数器与负载活动。战役报告用 SHA-256 绑定两个源报告，并给出 min/最近秩中位数/max 观测包络及带符号中位数差值。它刻意没有延迟 PASS/FAIL 阈值，也不能从托管 runner 数据建立 SLA。

默认输出文件：

- `/tmp/wbcan-latency-idle.json`
- `/tmp/wbcan-latency-controlled-load.json`
- `/tmp/wbcan-latency-campaign.json`

独立验证已保存的证据：

```bash
python3 kernel/wbcan/test_latency.py --validate-report /tmp/wbcan-latency-idle.json
python3 kernel/wbcan/test_latency.py --validate-report /tmp/wbcan-latency-controlled-load.json
python3 kernel/wbcan/validate_latency_campaign.py \
  --validate-report /tmp/wbcan-latency-campaign.json
```

把来自不同内核、commit、CPU 亲和性、CAN ID、采样预算或期限的报告作为独立战役保存。不要合并它们的百分位，也不要把虚拟比较呈现为物理或实时资质。
