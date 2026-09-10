# 外部冷启动协议

验收要求三名参与者中至少两人在干净机器上 60 分钟内达到健康的看板。

## 参与者记录

将 [`cold-start-results.template.json`](cold-start-results.template.json) 复制为私有证据文件，并在每次干净机器运行后填写。用以下命令验证完成的文件：

```bash
python tools/scripts/validate_cold_start.py runs/evaluation/cold-start-results.json
```

该命令在至少三名真实参与者中的两人通过前会刻意失败。检入的模板不是证据。

| 字段 | 值 |
|---|---|
| 参与者 ID | |
| 操作系统与版本 | |
| CPU / 内存 | |
| Docker 版本 | |
| 开始时间 | |
| 首次 `/healthz` 200 时间 | |
| 首次 `/readyz` 200 时间 | |
| 已用分钟数 | |
| 结果 | pass / fail |
| 阻断日志引用 | |

## 被测路径

```bash
git clone https://github.com/Quchaosheng/workbench-desk-robot.git
cd workbench-desk-robot
docker compose up --build -d
curl --fail http://127.0.0.1:8080/healthz
curl --fail http://127.0.0.1:8080/readyz
```

不要预装仓库依赖、复用既有镜像，或在公开 README 之外帮助参与者。保留失败记录；它们是发布证据，不是要清理的分数。
