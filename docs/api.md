# API 参考

版本化的 HTTP 契约以 [`api-openapi-v1.json`](api-openapi-v1.json) 形式检入。`GET` 是唯一的 API 方法；`POST`、`PUT`、`PATCH` 和 `DELETE` 返回 `405 read_only`。服务在 `/api/v1/*` 上输出 `Content-Type: application/json; charset=utf-8`、`Cache-Control: no-store`、`X-Content-Type-Options: nosniff` 和 `X-API-Version: 1`。

`/api/*` 是 v1 的兼容别名。本地与拆分主机的客户端使用相同的 `/api/v1` 路径。运行与事件响应上限为 4 MiB；源文件上限为 10 MiB，每次运行最多 10,000 个事件。无效的运行标识符返回 `400`，未知运行返回 `404`，格式错误的源返回 `503`，超大的响应返回 `413`。没有端点会写入、控制或确认任务。

兼容性政策在 v1 内是增量式的。未来的 v2 必须通过 `/api/v2` 引入；别名不会被静默改指。

Python API 页面在 MkDocs 构建期间由源码 docstring 生成。

::: workbench.kernel.lifecycle

::: workbench.kernel.communication
