# tasks

任务定义：每种任务类型一个目录。每个任务有目标描述、一个验证器和一组场景。

```
tasks/
  pick_place/    put object A into container B (v0.1 demo task)
  kitting/       assemble a kit from a parts tray
  inspection/    check N attributes of an object (present, colour, orientation)
  assembly/      connect two parts in a defined configuration
```

新增任务：
1. 在 `tasks/<name>/verifier.py` 写一个验证器，接收 `WorldState` 并返回 `VerificationResult`
2. 向 `sim/scenarios/frozen/` 添加至少 3 个冻结场景
3. 向 `interfaces/examples/` 添加任务描述
4. 为验证器写单元测试

验证器是每个任务唯一变化的部分。其余（规划、执行、事件库、回放）全部复用。
