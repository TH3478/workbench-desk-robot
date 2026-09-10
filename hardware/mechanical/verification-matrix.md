# 机械验证矩阵 — Revision D

| 任务 | 证据 | 状态 / 闸门 |
|---|---|---|
| MECH1 | Revision D SCAD、升起稳定装配 STEP、降低导航 STEP，以及实测 1100/1450 mm 包络 | ENGINEERING COMPLETE；需要实物基准检验 |
| MECH2 | `generated/bom.csv`、升降锁、制动与工具基准零件 | COMPLETE |
| MECH3 | 260 x 105 x 128 mm 烟熏玻璃头部、228 x 92 圆角显示屏、防呆颈部定位座 | COMPLETE |
| MECH4 | 四个转向驱动模块、防护底座裙边、收起/展开的 820 x 820 mm 支撑状态与 350 mm 升降 | ENGINEERING COMPLETE；需要实物适配与驱动测试 |
| MECH5 | `drawings/thermal-flow.svg`、隔离电子器件与食品工具热区 | ENGINEERING COMPLETE；热测试属外部事项 |
| MECH6 | 分析与跌落筛选 JSON、28 mm 吸能层、20 g 筛选 | SCREEN COMPLETE；非线性 FEA/跌落属外部事项 |
| MECH7 | 两组七个关节 ID/限位、共享工作空间与爆炸 STEP | DIGITAL CHECK COMPLETE；防护型运动属外部事项 |
| MECH8 | 十个 D 版零件 STEP 文件、SCAD 源与 BOM，包括颈部支架 | READY FOR PROTOTYPE QUOTE |
| MECH9 | 快换工具接口、力/滑动/工具 ID 要求 | DIGITAL CHECK COMPLETE；工具测试属外部事项 |
| MECH10 | `analysis.json` 中的重心、行驶/稳定防倾筛选与机械臂力矩 | ANALYTICAL PASS；不是发布证据 |
| MECH11 | 升降双编码器、制动器、锁销、硬限位与夹挤传感器 | DESIGN COMPLETE；需要同步测试 |
| MECH12 | 快递、清洁与受监督电磁烹饪任务边界 | CONCEPT ONLY；需要实物验证 |
| MECH13 | 设计规格中的 CMF 与隐藏分模策略 | DESIGN COMPLETE；需要 DFM 与表面处理样件 |
| MECH14 | 移动底座 STEP 与全角度可见渲染中的四个转向驱动轮/叉/轴承组件 | DIGITAL CHECK COMPLETE；需要自主导航软件与实物驱动测试 |
| MECH15 | `REV-D-MASS-001`、稳定组件 ID、来源/哈希绑定、遗留 55 kg 迁移与 Owner 批准登记册 | ANALYTICAL CHECK COMPLETE；需要四位 Owner 批准与序列化称重 |
| MECH16 | 收起、升起、负载、共享工作空间、急停与稳定支脚展开位姿的 +X/-X/+Y/-Y 防倾筛选 | ANALYTICAL SCREEN COMPLETE；需要拉拔、坡道、制动保持、急停与稳定支脚实物测试 |

## 装配与公差基准

- 基准 A：移动底座顶框；平面度 0.30 mm。
- 基准 B：升降柱中心平面；导轨平行度在行程范围内不超过 0.20 mm。
- 基准 C：左/右肩部安装板；对称度与臂基座位置相对 B 的公差为 0.30 mm。
- 原型通用公差：ISO 2768-m；打印件 +/-0.30 mm。
- 在通电负载作业之前，必须测试升降导轨/锁间隙、制动保持、同步误差、夹挤检测
  与急停。
- 两条主臂组均使用内部线缆与受控的 4–6 mm 阴影间隙。工具表面使用可拆卸的
  316L/PEEK/硅胶零件；明火与热液搬运仍然被禁止。
