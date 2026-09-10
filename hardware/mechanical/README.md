# 机械工程包

本目录是 Workbench Home Robot Revision D 的受版本控制的机械概念。它不再是
280 x 240 x 330 mm 的桌面外壳。当前目标是一个 540 x 520 mm 移动底座，配 350 mm
带制动可升降躯干、连续一体的矿物白实用机身、两条七轴机械臂、一个 18 L 快递舱
与一套带锁定快换的工具系统。

## 权威基线

`design-spec.json`、`cad/desk_robot.scad` 与生成的 STEP 包使用同一坐标系：
`robot_base` 的 +X 指向机器人右侧、+Y 指向机器人后方、+Z 朝上，地面为 Z=0。
底座顶面为 Z=140 mm，已检入的装配体为升起的工作位姿，头部顶面包络为 1450 mm。
收起姿态的头部顶面包络为 1100 mm，受控升降行程为 350 mm。生成的几何在导出时
测量，若 STEP 顶面与受控包络不符，生成器将报错。

`design-spec.json#components` 是 Revision D 质量模型的唯一权威来源
（`REV-D-MASS-001`）。每个组件都有稳定的 ID、以 `robot_base` 为系的 kg/mm 数值、
来源、不确定度、状态与纳入规则。当前的 `mass-ledger.csv` 是经校验的镜像；
`mass-ledger-legacy.csv` 保留已被取代的 55 kg 计划行，绝不纳入发布计算。任何
发布决策之前，都明确要求在 `mass-model-approval-register.csv` 中完成 Product、
Mechanical、Hardware 与 Safety 批准。

Revision D 底盘由四个独立的 140 mm 转向驱动模块组成。任何描述两个 200 mm 驱动
轮、四个支撑脚轮、早期 55 kg 工作台或 Rev B/Rev C 固定外形的文档，都属于遗留的
计划参考资料，不得用作当前的机械或采购接口。

## 产品架构

- **移动底座：**四个独立的转向驱动模块提供纵向、横向、斜向与原地旋转的自运动。
  每个模块都有一个 140 mm 无痕轮、绝对转向编码器、驱动编码器、30 mm 悬架与
  常闭制动器。稳定支脚在导航时保持收起，仅在静止操作时伸出。
- **升降机构：**四根导轨、两根同步丝杠、两个常闭制动器、两个机械锁销、双编码器、
  硬限位与夹挤检测。
- **机械臂：**每条臂有 J1 基座偏航、J2 肩部俯仰、J3 肩部横滚、J4 肘部俯仰、
  J5 前臂横滚、J6 腕部俯仰与 J7 工具横滚。单臂规划包络为 650 mm 臂展下 2 kg，
  或降速下 400 mm 处 3 kg；这些还不是经认证的性能指标。
- **工具：**自适应快递夹爪、柔顺毛刷/干拖头，以及可拆卸的 316L/PEEK/硅胶电磁炉
  烹饪工具。烹饪须有人监督、仅限电磁加热，且排除明火、沸液搬运与热锅移动。

## 工业设计与 CMF

面向消费者的表面是一体连续的暖矿物白外壳，主分模线被隐藏。结构腰部、升降机构
与臂杆使用喷砂石墨色阳极氧化铝；面部为单块烟熏强化玻璃镜片；快递舱/声学嵌件
为石墨色 3D 针织再生 PET。玉色或暖琥珀色只留给一个状态灯。可见的亮面塑料、
外露紧固件、装饰色块、玩具式天线与无防护的轮机构都不在范围内。四个轮面保持
视觉可读，让产品清晰地传达自运动能力，而转向轴承与线缆仍收纳在底座裙边内部。

头部在柔和的白色边框内使用宽大的圆角矩形表情窗口。它不是一个悬浮外壳：专用的
颈部支架带有承重底座、宽阔的肩板、防呆的头部定位座、四个隐藏 M6 紧固件、两个
定位销与一条 32 mm 中央走线通道。连接线束后，将头部抬上定位座；松开后盖与
底部紧固件后即可垂直取下。两个肩部中心安装在颈部下方的躯干侧壁中；两条臂都
不支撑、也不在视觉上框住头部。

## 复现

```bash
python hardware/mechanical/tools/generate_artifacts.py
```

该命令重新生成分析报告、C 版总布置图、热路径、跌落筛选、BOM、装配顺序与
CadQuery STEP 包。它有意报告 `CONCEPT_PHYSICAL_VALIDATION_REQUIRED`：任何渲染
或分析结果都不能替代序列化样机上的升降同步、机械臂扫掠、热、稳定性、力限制
或防护型家务任务测试。

- `generated/enclosure.step`：供供应商评审的躯干替换实体。
- `generated/desk_robot_assembly.step`：升起的工作位姿，含展开的稳定支脚、移动
  底座、升降机构、躯干、头部、双 7R 机械臂与工具坞。
- `generated/desk_robot_navigation_low.step`：降低的导航位姿，稳定支脚收纳于
  底座包络内。
- `generated/desk_robot_exploded.step`：用于作业指导书的爆炸装配图。
- `generated/parts/*.step`：十个 D 版概念零件，包括独立的颈部支架。
- `generated/drawings/general-arrangement.svg`：D 版架构与升降状态。
- `generated/drawings/thermal-flow.svg`：隔离电子器件的气流路径。
- `generated/analysis.json`：带哈希的质量模型、分位姿重心、四方向行驶/稳定支脚
  防倾筛选、负载力矩与净空。
- `analysis.schema.json`：生成分析证据的契约。
- `generated/bom-manifest.json`：生成 BOM 的质量模型版本/哈希绑定。
- `revision-d-architecture.md`：双臂工作空间、任务边界与架构依据。
