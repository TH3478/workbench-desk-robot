# OmniLink 集成

这是一个可选的 HTTP 适配器，连接单独托管的 [OmniLink AI](https://github.com/vivekmaru/omnilink-ai) 实例。它增加知识搜索、RAG 问答与 Workbench 运行摘要的单向导出。

## 快速开始

```python
from integrations.omnilink import OmniLinkClient, RunSummaryExporter

client = OmniLinkClient("http://127.0.0.1:3000")
results = client.search("gripper calibration")
answer = client.ask("Which calibration notes mention the gripper?")
RunSummaryExporter(client, "http://127.0.0.1:8080").export(run_summary)
```

让 OmniLink 留在它自己的进程/容器与数据库中。适配器绝不发送原始事件流、`TaskGraph`、`SemanticAction`、动作结果、相机数据或安全状态。OmniLink 不可用不得阻塞 Workbench 的离线/控制路径；在可选的调用点捕获 `OmniLinkError`。

部署前，把 OmniLink 绑定到私有接口，在前面放置认证/TLS/速率限制，对出站 URL 做白名单，钉定 OmniLink 的 commit/镜像摘要，并评审其许可证与 Gemini 数据处理政策。
