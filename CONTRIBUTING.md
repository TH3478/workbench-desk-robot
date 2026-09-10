# 贡献指南

## 编码之前

1. 使用带有一位人类 Owner 与验收标准的 Ready Issue。
2. 确认所属路径与接口 schema。
3. 从当前 `main` 分支切出分支。

## 分支命名

```text
feat/<issue>-<short-name>
fix/<issue>-<short-name>
test/<issue>-<short-name>
docs/<issue>-<short-name>
chore/<issue>-<short-name>
```

## 提交信息

使用 Conventional Commits 规范并附带 DCO 签署：

```text
feat(world-model): add deterministic tray verifier

Signed-off-by: Your Name <you@example.com>
```

## Pull Request

- 每个 PR 只做一件事。
- 除非必要，不要把接口、功能与格式重写混在一起。
- 附上确切的命令与结果。
- 邀请模块 Owner 与每一位受影响的契约消费者评审。
- 严禁强推（force-push）或直接向受保护的 `main` 提交。

## 必做的本地检查

```bash
make test
make contract
make scenario-check
make context-check
make demo-scripted
```

ROS、仿真与固件相关 PR 还需附加各自路径的专项检查。截图只能作为 UI 外观的证据，不能作为物理任务完成或安全的证据。
