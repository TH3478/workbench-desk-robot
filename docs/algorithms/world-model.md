# 世界模型算法

世界模型如何决定什么是真实的，以及何时判定自己无法判断。

本文是技术参考。任务拆解与人员配置在仓库之外。

---

## 为什么本模块承载项目的主张

项目只主张一件事：*命令已被发送，不等于目标已经达成。*

其他每个模块都可以报告“我这部分完成了”。本模块必须报告
“目标已达成，这是证据”——或“我无法判断，这是缺失的部分”。

难点不在代码，而在于让**“我无法判断”成为一种有依据的结论，而不是借口**。
这需要真正的算法：多假设跟踪、冲突处理、信念衰减、确定性重建。

---

## 1. 确定性状态重建

同一事件流回放两次，必须产生相同的 `state_hash`。三种常见的错误做法：

```python
# Wrong: dict ordering is not stable across runs
h = hashlib.sha256(json.dumps(state).encode())

# Wrong: float mantissa noise. 0.1 + 0.2 can differ across platforms
h = hash((entity.x, entity.y, entity.z))

# Wrong: a timestamp inside the hash
state["updated_at"] = datetime.now()
```

正确做法：

```python
def state_hash(s: WorldState) -> str:
    canonical = {
        "run_id": s.run_id,
        "entities": sorted((e.id, quantize(e.pose), confidence_bucket(e.confidence)) for e in s.entities),
        "relations": sorted(s.relations),
    }
    return hashlib.sha256(json.dumps(canonical, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def quantize(p: Pose, eps: float = 1e-6) -> tuple:
    """Pose quantisation.

    eps is NOT final. It must come from the measured pose jitter of a
    stationary object on real hardware (see section 6). Until then this is an
    estimate, and it is a config value rather than a literal for that reason.
    """
    return tuple(round(v / eps) * eps for v in (p.x, p.y, p.z, *p.quat))


def confidence_bucket(c: float, width: float = 0.05) -> int:
    """0.9001 and 0.9002 must not produce different hashes."""
    return int(c / width)
```

`confidence_bucket` 是最容易被跳过的一环。原始置信度是浮点数；不对其分桶就直接参与计算，哈希会每一帧都变。

### 快照

十分钟的任务会产生数万个事件。对于每次请求都要回放的 UI 来说，完整回放太慢。

```
Snapshot every N events (start with N=500).
Replay request -> nearest snapshot -> apply the delta.

Required invariant:
    hash(restore(snapshot) + replay(delta)) == hash(replay(everything))
```

该断言是全部要点所在。与完整回放不一致的快照就是在捏造状态。

### 因果顺序，而不是时间戳顺序

动作结果携带的时间戳可能*早于*触发它的观测——不同节点、不同时钟偏移。

```python
events.sort(key=lambda e: e.timestamp)  # wrong
events.sort(key=lambda e: e.sequence_no)  # right
```

`sequence_no` 建立全序。时间戳仅用于诊断，绝不参与状态转换决策。这是 reducer 的不变量，需要测试：构造一条时间戳倒序而 `sequence_no` 正序的事件流，然后断言状态仍然正确。

| 字段 | 时钟 | 原因 |
|---|---|---|
| `sequence_no` | 无（单调整数） | 全序 |
| `monotonic_ns` | 单调时钟 | 间隔、超时。NTP 步进不影响它 |
| `wall_clock` | 墙钟 | 人类可读日志、跨机器对齐 |
| `state_hash` 中的任何字段 | **排除** | 时间永不进入哈希 |

超时逻辑必须使用单调时钟。使用墙钟时，一次 NTP 校正就可能误触发或压制看门狗。

### Bounded observed attributes

Observed attributes follow the same evidence-first rule as pose and location:
the value is useful only when the system can identify when, how confidently,
and from which evidence it was observed. The public contract therefore carries
three related fields on an entity or observation:

```json
{
  "attributes": {"condition": "intact"},
  "attributes_mode": "complete",
  "attributes_schema_version": "observed-attributes-v1",
  "attribute_metadata": {
    "condition": {
      "observed_at": "2026-08-25T00:00:00Z",
      "confidence": 0.95,
      "evidence_refs": ["frame://attribute/001"],
      "belief": "observed",
      "clock_id": "wall",
      "source": "camera-01"
    }
  }
}
```

The vocabulary is finite and entity-scoped. Common entities may report
`colour`, `presence`, `identity`, and `orientation`; parcel entities may also
report `label_status`, `condition`, `tracking_id`, `barcode`, and `parcel_uid`;
appliances and managed slots use the documented door/rack and slot keys. An
unknown entity type receives only the common keys. Values are strings, with a
maximum of 32 entries, 64 characters per key, 256 characters per value, and
4096 bytes of canonical UTF-8 JSON. Text must be printable, valid UTF-8, and
free of surrounding whitespace. Enumerated keys are validated against the
finite values in `workbench_contracts.observed_attributes`.

Metadata is bounded independently: at most 32 keys and 16 KiB of canonical
UTF-8 JSON; timestamps, sources, and evidence references are bounded text;
evidence has one to 32 unique references; confidence is finite and in `[0, 1]`;
and belief is one of `observed`, `inferred`, `stale`, or `lost`. Modern
`observed-attributes-v1` payloads require metadata for every attribute. The
explicit `legacy-observed-attributes-v0` marker is a compatibility path for
older parcel events, not a way to introduce unknown keys or unbounded data.

Reducer update semantics are intentionally asymmetric:

| Update | Meaning | Required invariant |
|---|---|---|
| `complete` | Replace the entity's attribute values and metadata; omitted keys are removed. | Establishes or resets the complete baseline. |
| `partial` | Merge only the named keys and retain other baseline keys. | A prior complete baseline must exist; older per-key metadata cannot overwrite newer metadata. |

The reducer replays these updates by `sequence_no`, ignores provably older wall
timestamps, and treats duplicate event IDs idempotently. A schema-version
conflict is rejected except for an explicit legacy-to-modern complete migration.
`ActionResult` events never enter this attribute update path, so a reported
action cannot manufacture observation truth.

Attribute metadata ages with the same explicit replay boundary as other world
facts. Callers provide both an `ObservationFreshnessPolicy` for the exact
`(source, entity_type)` pair and an `ObservationAgingBoundary`; the aging code
does not read process time or apply a wildcard fallback. Comparable wall-clock
metadata becomes `observed`, then `stale`, then `lost` at the configured
thresholds. Missing, future, incomparable, or unconfigured timestamps are
`lost`; an `inferred` value remains inferred while its observation is fresh.

The canonical public `WorldState` projection includes attribute values, their
schema version, metadata, and evidence in `state_hash` material. Snapshot
timing metadata such as `reduced_at` is excluded from the semantic hash. This
keeps replay integrity sensitive to a changed observed fact while avoiding a
hash change caused only by when a snapshot was serialized.

---

## 2. 多假设跟踪

### 为什么单一估计不够

一只手遮挡了模块。最后一次直接观测把它放在 A 点。手移开后，它可能已被推到 B 点。

单一估计模型报告“在 A”——把陈旧信息当作事实呈现。虚假完成正是由此而来。

多假设模型保留：

```
module_red: {
    pose_A:  0.60,   # last direct observation
    pose_B:  0.30,   # implied by the hand's trajectory
    unknown: 0.10
}
```

没有假设超过阈值，因此验证返回 `insufficient_evidence`，规划器重新观测。

**没有多假设时，`insufficient_evidence` 只能在“从未观测到”时触发**——这漏掉了更常见的情况：“观测到了，但不确信”。

### 数据关联

三个颜色相同的模块；新一帧显示三个红色方块。谁是谁？

```python
from scipy.optimize import linear_sum_assignment


def associate(observations, tracks, w1=1.0, w2=0.5, w3=0.2):
    C = np.zeros((len(observations), len(tracks)))
    for i, obs in enumerate(observations):
        for j, tr in enumerate(tracks):
            C[i][j] = (
                w1 * pose_distance(obs.pose, tr.predicted_pose)
                + w2 * (1.0 - appearance_similarity(obs, tr))
                + w3 * time_penalty(obs.stamp, tr.last_seen)
            )

    row, col = linear_sum_assignment(C)

    matched, new = [], []
    for i, j in zip(row, col):
        if C[i][j] < COST_THRESHOLD:
            matched.append((observations[i], tracks[j]))
        else:
            new.append(observations[i])  # too expensive: new instance
    lost = [t for k, t in enumerate(tracks) if k not in col]
    return matched, new, lost
```

算法本身只是一次库调用。**工作在于标定 w1/w2/w3 与 `COST_THRESHOLD`**，使它们在遮挡、光照变化、快速运动与外观几乎相同的情况下依然正确。使用标注好的真值集作为验证集。

凭直觉挑选这些权重是这类任务失败的常见方式。

### 剪枝

假设逐帧逐实体倍增，因此需要上限。

```
Cap: <=5 hypotheses per entity.

Prune in order:
  1. drop anything below 0.05 confidence
  2. merge hypotheses closer together than the quantisation step
     (they are the same hypothesis)
  3. still over the cap -> keep the top 5, fold the rest into `unknown`
```

第 3 步必须**把剪掉的概率质量加进 `unknown`**，而不是丢弃。丢弃会让总和不再等于 1，验证器的阈值比较就失去了意义。

---

## 3. 信念衰减

衰减太快，静止物体跌入未知，系统就会永远重复观测，任务永远无法完成。衰减太慢，被移动物体的陈旧位姿就会被当作事实，从而产生虚假完成。

半衰期必须按物体类型区分：

```python
HALF_LIFE_S = {
    "tray": 600.0,  # fixture. Position does not change unless moved
    "table": 600.0,
    "module": 15.0,  # can be moved by the arm or a person
    "gripper_tip": 0.5,  # always moving; last frame is already stale
}


def decay(conf: float, elapsed_s: float, kind: str) -> float:
    hl = HALF_LIFE_S[kind]
    return conf * (0.5 ** (elapsed_s / hl))
```

这张表本身就是交付物——一个全局常量行不通。

`gripper_tip` 的 0.5 s 最为关键：给它托盘的半衰期，验证器就会拿几秒前的夹爪尖端位姿来决定某物当前是否被握住。

---

## 4. 冲突证据

两次观测把同一个物体放在了不同位置。

| 策略 | 适用 | 失效场景 |
|---|---|---|
| 新者胜 | 物体确实在移动 | 一次误检覆盖了好数据 |
| 置信度最高者胜 | 传感器质量有差异 | 一条自信但陈旧的数据打败了新鲜但不确定的数据 |
| 主传感器胜 | 存在明确的主传感器 | 主传感器失效且没有后备 |

### 本模块的选择：不消解

```python
def resolve(obs_a, obs_b) -> Belief:
    if pose_distance(obs_a.pose, obs_b.pose) > CONFLICT_THRESHOLD:
        return Belief(
            status="conflicting_observations",
            hypotheses=[
                Hypothesis(obs_a.pose, obs_a.confidence, obs_a.evidence_refs),
                Hypothesis(obs_b.pose, obs_b.confidence, obs_b.evidence_refs),
            ],
            recovery_hint="reobserve",
        )
    return merge(obs_a, obs_b)  # merge only when they agree
```

与三值验证同理：**系统不猜测。** 强行消解*就是*猜测，而错误的猜测不留痕迹——日志显示一个自信的结论，却没有任何迹象表明它是在两个互相矛盾的读数中挑出来的。

之所以把推理过程写下来，是因为下一个维护者的第一反应会是加一条自动二选一的规则。那会掩盖系统本来就知道的不确定性。

---

## 5. 验证

### 包含判断是三值的，不是二值的

中心落在托盘边缘上的模块算“在里面”吗？

```python
def containment(module_aabb, cavity_aabb) -> VerificationStatus:
    inter = intersect_volume(module_aabb, cavity_aabb)
    ratio = inter / volume(module_aabb)

    if ratio >= 0.95:
        return CONFIRMED  # fully contained
    if ratio <= 0.01:
        return REFUTED  # fully separate
    return INSUFFICIENT_EVIDENCE  # partial: caught on the rim
```

中间区间正是要点所在。二值测试会迫使“卡在托盘边缘”被归为成功或失败，而它实际的含义是*放置不当，重试*——`recovery_hint = retry_action`。

这也是为什么托盘必须建模为带真实内部空腔的五个部件，而不是一块平板。平板没有 `cavity_aabb`，这个测试就无从下手。

### 阈值标定不是为了优化准确率

取标注好的真值帧，绘制 ROC，然后**选取假阳性为零的阈值。**

假阳性 = 报告完成但实际未完成 = 虚假完成 = 发布阻断项。

代价是更多的假阴性（实际完成，报告为不确定）。**该取舍方向是项目的核心主张**，绝不能为了让完成率数字更好看而反转。

```
Objective:   FP = 0, minimise FN subject to that
Not:         maximise (TP + TN) / total
```

### 失败分类

十个 `reason_code` 值，各自映射到一个恢复动作：

| `reason_code` | 含义 | `recovery_hint` |
|---|---|---|
| `target_not_observed` | 从未观测到 | `reobserve` |
| `target_lost` | 曾观测到，现在消失 | `reobserve` |
| `belief_stale` | 观测已过期 | `reobserve` |
| `confidence_below_threshold` | 观测到了，但置信度不足 | `reobserve` |
| `conflicting_observations` | 观测互相矛盾 | `reobserve` |
| `partial_containment` | 卡在边界上 | `retry_action` |
| `precondition_unmet` | 动作前置条件为假 | `replan` |
| `action_reported_failure` | 动作层报告失败 | `retry_action` |
| `geometry_mismatch` | 位姿与预测相差甚远 | `replan` |
| `timeout_no_evidence` | 超时且没有新证据 | `ask_confirm` |

前五个都映射到 `reobserve`，但保留不同代码。这不是冗余——它们在指标报告中被分别计数，这正是你了解系统实际卡在哪里的方式。

---

## 6. 真机噪声决定量化步长

真实相机观察静止物体时，每一帧报告的位姿都不同。在确定第 1 节的量化步长之前，先测量抖动。

```
1. Fix a module. Do not move it.
2. Capture 1000 frames.
3. Compute the per-axis standard deviation of the pose.
4. Quantisation step = 3σ (covers 99.7% of the jitter).
```

然后回填 `quantize()` 中的 `eps` 并**重新运行哈希一致性测试**——参数变了，证据就必须重新生成。

这条依赖从早期仿真工作延伸至真机工作，也是本模块中最容易丢失的一环。`quantize()` 的 docstring 正是为此而写。

---

## 不变量

1. **只有本模块决定任务完成。** 任何其他模块都不得断言“任务成功了”。
2. **缺失证据绝不用默认值填补。** 缺失就是缺失；记录 `reason_code`。
3. **Oracle 字段永远不能到达 reducer 或验证器。** 评估真值只存在于评估模块。用 Oracle 数据计算感知分数会使整个运行作废。

---

## 指标

| 指标 | 目标 |
|---|---|
| `state_hash` 一致性 | 100% |
| **虚假完成** | **0**（发布阻断项） |
| 验证携带证据引用 | 100% |
| 固定任务回放成功率 | ≥95% |
| 事件库吞吐量 | >1000 事件/秒 |
| 每实体假设数 | ≤5 |
| 数据关联准确率（验证集） | ≥90% |
