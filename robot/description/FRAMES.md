# 坐标系与尺寸

`workbench.urdf.xacro` 里的每个数字以及它为什么是这个数。不读本文件就改尺寸，下游某处会无声地坏掉。

---

## 坐标系树

```
world
└── table                    slab centre
    └── table_surface        the working plane — express scenario poses here
        ├── tray_base        tray origin, on the surface
        │   ├── tray_floor
        │   ├── tray_wall_xp / xn / yp / yn
        │   └── tray_cavity  ← containment is computed against this
        ├── module_red       free body; pose comes from the scenario manifest
        └── camera_post
            └── camera_body
                └── camera_optical   REP-103: z fwd, x right, y down
```

机械臂挂在 `world` 上，并在启动时组合。它不在本文件里。

---

## `table_surface` 为什么存在

场景位姿相对于 `table_surface` 表达，而不是 `table`。

`table` 是桌面的*中心*，因此相对它的位姿依赖于 `table_thick`。把桌面从 40 mm 改成 30 mm，每个清单里的每个物体会平移 5 mm——清单仍然校验通过，所有场景都微妙地错了，而且没有任何东西报告它。

`table_surface` 位于工作平面上。相对它放置的物体在桌面厚度变化时保持不动。

---

## 尺寸

| 属性 | 数值 | 理由 |
|---|---|---|
| `table_x` × `table_y` | 1.20 × 0.80 m | 容纳 850 mm 臂展机械臂的工作空间，四周留有余量 |
| `table_thick` | 0.04 m | 仅结构用途。没有东西依赖它，因为位姿都经 `table_surface` |
| `table_height` | 0.75 m | 标准工作台高度；与真机机械臂基座安装位置一致 |
| `tray_x` × `tray_y` | 0.24 × 0.18 m | 可容纳若干 40 mm 模块，并为接近误差留出间隙 |
| `tray_wall` | 0.006 m | 薄到不占据内部空间，厚到足以稳定接触 |
| `tray_depth` | 0.05 m | 比模块高度更深，因此放入的模块明确在内部 |
| `tray_floor` | 0.004 m | 给腔体一个真实的底面可搁置 |
| `module_size` | 0.040 m | 在标准平行夹爪行程之内，留有接近裕量 |
| `module_mass` | 0.050 kg | 足够轻，硬接触会把它弹飞——刻意为之，见下文 |
| `cam_height` | 0.70 m | 一帧内同时看到模块起始区域与托盘 |

### 模块为什么刻意做得轻

50 g 意味着调得不好的接触模型会把它甩过桌面。

这正是物理调参存在的意义——消除这类失败模式。把模块做重会掩盖糟糕的接触参数——抓取会因错误的原因成功，而同样的参数会在物体确实很轻的真机上失败。

---

## 托盘由五个部件组成

底面加四面墙，而不是一个盒子。

验证器通过相对 `tray_cavity` 的空间包含判定「模块是否在托盘里」。这需要内部体积。单个盒子没有内部——对实体的包含要么是与实体本身相交（无意义），要么什么都没有。

### 内部尺寸

消费者需要这些值。它们是推导出来的，不是硬编码：

```
x: tray_x    - 2 * tray_wall  = 0.240 - 0.012 = 0.228 m
y: tray_y    - 2 * tray_wall  = 0.180 - 0.012 = 0.168 m
z: tray_depth -    tray_floor = 0.050 - 0.004 = 0.046 m
```

`tray_cavity` 的原点在该体积的中心。

**从模型读取它们，而不是用 Python 里的常量。** 验证器里硬编码的 0.228 会在有人第一次在这里加宽托盘时过时，而由此产生的失败看起来会像感知问题。

---

## 包含判定是三值的

| 重叠比 | 状态 | 含义 |
|---|---|---|
| ≥ 0.95 | `confirmed` | 完全在内 |
| 0.01 – 0.95 | `insufficient_evidence` | 卡在边缘——重试动作，不要重新规划 |
| ≤ 0.01 | `refuted` | 在外 |

中间区间就是腔体必须几何上真实存在的原因。布尔判定把「卡在托盘边缘」强行归为成功或失败，而它实际意味着*放得不好，重试该动作*。

完整推理：`docs/algorithms/world-model.md`。

---

## `camera_optical` 不是 `camera_body`

两个坐标系，相差 90°，混淆它们是经典的无声 bug。

| 坐标系 | 约定 |
|---|---|
| `camera_body` | 物理盒体。x 按机体向前 |
| `camera_optical` | REP-103。**z 向前，x 向右，y 向下** |

ROS 图像流水线和每个标签检测器都假设光学约定。若以机体坐标系发布检测结果，位姿会旋转 90°。

它在 RViz 里看起来仍然合理。没有任何报错。检测结果只是在错误的位置，第一个症状是抓取以一个稳定的偏差落空，然后被归咎于标定。

**目视验证一次。** `check_urdf` 抓不住这一点——它校验结构，而不校验旋转是否物理合理。

---

## 相机噪声刻意非零

`stddev = 0.007`。

无噪声相机让感知以真机上会立刻失败的置信度阈值通过。检测在仿真里看起来已解决，却在硬件启动调试时崩溃，而且无法判断回归来自相机、光照还是检测器。

在测量真机相机抖动之前，这个值只是占位。同一测量还供 `docs/algorithms/world-model.md` §6 的位姿量化步骤使用。

---

## 摩擦系数会变

| 连杆 | `mu1` / `mu2` | 备注 |
|---|---|---|
| `module_red` | 0.8 | 塑料对塑料，起始值 |
| `tray_floor` | 0.6 | |
| `table` | 0.7 | |

这些数字是物理调参会动的值。它们起步合理，预期会变。

**调参规则：一次只动一个参数。** 摩擦与求解器设置都影响抓取成功率。两次运行之间两者都改，结果就无法归因。这就是物理调参以周计、无法由两个人独立并行推进的原因。

---

## 本文件不包含什么

| 不在本文件 | 位置 | 原因 |
|---|---|---|
| 机械臂 | 官方厂商包，启动时组合 | 让世界与机械臂可以并行构建 |
| 关节限位、控制器 | `robot/control/` | 不同 Owner，不同评审路径 |
| 场景物体位姿 | `sim/scenarios/frozen/*.yaml` | 按 run 播种；同一种子必须重建同一场景 |
| 逐位精确的相机内参 | 标定输出，真机 | 仿真内参不是真实内参 |

---

## 检查

```bash
# Expand and validate structure
xacro robot/description/workbench.urdf.xacro > /tmp/wb.urdf
check_urdf /tmp/wb.urdf

# Look at the tree
urdf_to_graphiz /tmp/wb.urdf

# Then look at it in RViz and confirm camera_optical points at the table.
# This is the one thing the tools cannot check for you.
```

`check_urdf` 能抓住无父连杆和格式错误的关节。它抓不住转错方向的坐标系、与墙壁不对齐的腔体，或物理上不可能的惯量张量。

---

## 关于 CAD

ROS 不消费 SolidWorks 文件。流水线是 CAD → STL/DAE 网格 → 从 URDF 引用。官方机械臂包已同时发布 URDF 与网格。

这里的一切都是基本几何体，因此本阶段无需机加工。

若日后某部件确实需要机加工，顺序很重要：**先在 URDF 中锁定尺寸与坐标系，再按这些数字建模 CAD。** 反过来做会让关节轴与坐标系原点偏离软件已假定的值，事后调和比听上去更糟。
