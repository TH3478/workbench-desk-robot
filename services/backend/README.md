# 后端边界（9 人计划中由 World Model 拥有）

P0 是 SQLite 加一个薄 FastAPI 读取 API。不要在这里创建第二个 WorldState。初始 `SQLiteEventStore` 位于 `services/world_model/`，直到本服务需要按证据拆分。
