# 仿真

感知 Owner 拥有场景；集成 Owner 拥有世界资产。仓库目前包含确定性场景清单与一个受限的 runner 控制面，但**尚不**包含完整的 Gazebo 世界、感知桥接或抓取适配器。脚本化事件日志是流水线固定装置，绝不是硬件或 Gazebo 证据。

## 操作入口

从仓库根目录运行：

```bash
python tools/scripts/sim_cli.py doctor
python tools/scripts/sim_cli.py list
python tools/scripts/sim_cli.py run normal-001 --runner scripted --output-dir runs/sim
python tools/scripts/sim_cli.py run --all --runner gazebo
```

`doctor` 只诊断依赖。`list` 校验每个清单并显示确定性的物化场景 hash。`sim_cli run` 发布一个原子运行产物，包含源清单、物化场景、事件日志（存在时）、stdout/stderr、元数据与校验和。

默认 Gazebo runner 需要 `WORKBENCH_GAZEBO_COMMAND` 中已配置、令牌化的命令，或显式的 `--command` 参数。适配器缺失时记为 `NOT_EXECUTED` 且退出码非零。脚本化运行始终标记为 `SCRIPTED_FIXTURE` 与 `release_eligible: false`。

## 可复现性边界

同一清单与种子产生相同的物化场景与场景 hash。该保证不延伸到 Gazebo 事件顺序、物理时序、传感器噪声或硬件行为。原始 runner 日志与元数据被保留，使这些差异保持可检查。

## 所有权边界

不要为了让场景变简单而修改机器人控制逻辑。未来的真实世界 runner 必须应用清单种子、世界版本、故障类型、重置隔离与超时，然后发出既有的、已验证的事件日志契约。它不得把一个固定装置或缺失的依赖提升为通过的回归。
