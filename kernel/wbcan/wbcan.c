// SPDX-License-Identifier: GPL-2.0
/*
 * wbcan——支持可编程故障注入的虚拟 CAN 设备。
 *
 * vcan 是一条完美的导线：写进去的每一帧都会原样回来。真实 CAN 并非如此。
 * 控制器会进入 bus-off，TX 邮箱会填满，仲裁会失败，
 * 位错误会破坏负载。只见过 vcan 的固件从未执行过自己的错误路径。
 *
 * 本驱动就是 vcan 加上一个故障平面。你通过 debugfs 武装一个故障，
 * 接下来的 N 帧就会命中它，错误则以 CAN 核心预期的方式浮现：
 * 套接字上的错误帧、经过 CAN_STATE_* 的状态迁移、
 * 以及移动中的 TX/RX 错误计数器。因此被测固件看到的是
 * 一条逐渐变坏的总线，而不是一个特殊的测试 API。
 *
 * 供 firmware/mcu 任务 FW13（寄存器级故障注入）与
 * FW15 的 40 故障套件使用。参见 docs/decisions/ADR-0003-mcu-riscv-qemu.md。
 *
 * ── 模块职责与边界 ──
 * wbcan 是挂在 Linux 主机上的虚拟 CAN 网络设备（单例 wbcan0），在板上
 * MCU ⇄ 主机 SocketCAN 用户态的链路里扮演 CAN 控制器：用户态通过
 * AF_CAN / CAN_RAW 套接字收发 struct can_frame，本驱动按标准 SocketCAN
 * 语义提供 MTU、回环、软件时间戳、错误帧与状态迁移。它对应
 * docs/architecture/robot-bsp-can-v0.1.md「节点分配」一节的 Linux 网关
 * 节点（0x01）所在主机一侧；该文档「故障行为」一节的 bus-off 条款
 * （暴露 SocketCAN 重启状态、保留故障计数、恢复前保持抑制）正是本驱动
 * can_bus_off() 路径要呈现给用户态的语义。
 *
 * ── 透传语义 ──
 * wbcan 是帧级透传设备：它只搬运 struct can_frame，绝不解析或改写
 * Wire V1 载荷。版本字节 0x10、16 位大端 command_id、opcode、
 * retry_count、遥测 sequence_no 等字段（docs/architecture/mcu-wire-v1.md
 * 「载荷布局」一节）以及五类仲裁标识符 0x080 stop、0x081 stop_ack、
 * 0x100 command、0x101 ack、0x180 telemetry（「仲裁标识符」一节）对驱动
 * 都是不透明字节。协议编解码属于 MCU 固件 firmware/mcu/core/frame_codec.[ch]
 * 与主机适配器（docs/architecture/host-can-transport-v1.md「SocketCAN
 * 入口契约」一节）。故障注入只在帧层面操作（丢弃、翻转负载位、制造错误帧
 * 与状态迁移），不改变任何 Wire V1 协议数字或线上字节序。
 *
 * ── 对用户态传输适配器的承诺 ──
 * host-can-transport-v1.md「SocketCAN 入口契约」要求：错误帧以内核 CAN
 * 错误帧形式呈现且绝不进入 Wire V1 解码，bus-off 使适配器可观测地进入
 * BUS_OFF。本驱动用 can_change_state() 的状态迁移、alloc_can_err_skb()
 * 的错误帧与 can_bus_off() 的载波关闭提供这些语义；ethtool 的
 * .get_ts_info 与 skb_tx_timestamp()（软件发送时间戳）支撑该契约的
 * SO_TIMESTAMPNS 时间戳约定。
 *
 * ── 延迟测量契约 ──
 * kernel/wbcan/LATENCY.md「配置档」一节的 status-readers 档反复读取本
 * 驱动 debugfs 下的 status 快照；延迟探针只驱动本虚拟设备，其报告不是
 * 物理 CAN、控制器 IRQ、收发器、MCU、执行器、PREEMPT_RT 或硬实时证据。
 *
 * 为什么是内核模块而不是用户态程序：bus-off 状态、错误计数器与
 * 错误帧的生成都位于内核 CAN 核心中。用户态桥接器可以丢弃或篡改帧，
 * 却无法让 can_get_state() 报告 CAN_STATE_BUS_OFF，
 * 而这一迁移正是固件恢复路径所依赖的关键。
 */

/* 给 pr_warn() 等裸内核打印统一加模块名前缀；netdev_* 日志用设备名，
 * 不受此宏影响。 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/*
 * 头文件分层：ethtool 与 debugfs 支撑观测 ABI；can/dev.h、can/error.h、
 * can/skb.h 提供 CAN 核心设备抽象、错误帧编码与 skb 助手；netdevice.h
 * 提供 TX 队列生命周期原语；u64_stats_sync 支撑 64 位统计的无撕裂读取。
 */
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/u64_stats_sync.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/skb.h>

/*
 * 预留的 TX echo 队列深度（真实 CAN 控制器常见 4 槽邮箱规模）。本驱动
 * 当前未引用它：回环不走 can_put_echo_skb()/can_get_echo_skb() 邮箱路径，
 * 而是在 ndo_start_xmit() 内同步完成（wbcan_init() 以 echo_skb_max = 0
 * 调用 alloc_candev()）。
 */
#define WBCAN_ECHO_SKB_MAX	4

/*
 * ethtool 私有统计（ethtool -S wbcan0）的索引枚举。名称数组
 * wbcan_ethtool_stat_names[] 必须与本枚举逐位对应，二者共同定义
 * get_strings() 与 get_ethtool_stats() 的输出顺序；末尾的
 * WBCAN_ETHTOOL_STAT_COUNT 即 get_sset_count() 报告的数量。
 */
enum wbcan_ethtool_stat_index {
	WBCAN_ETHTOOL_TX_FRAMES,
	WBCAN_ETHTOOL_RX_FRAMES,
	WBCAN_ETHTOOL_FAULT_CANDIDATES,
	WBCAN_ETHTOOL_FAULT_INJECTED,
	WBCAN_ETHTOOL_BUS_ERRORS,
	WBCAN_ETHTOOL_ARBITRATION_LOST,
	WBCAN_ETHTOOL_RESTART_ATTEMPTS,
	WBCAN_ETHTOOL_STOP_ATTEMPTS,
	WBCAN_ETHTOOL_STAT_COUNT,
};

/* 统计名经 ethtool get_strings() 原样导出给用户态（ethtool -S 的键名），
 * 属于观测 ABI：改名会破坏解析这些键的脚本。 */
static const char *const wbcan_ethtool_stat_names[WBCAN_ETHTOOL_STAT_COUNT] = {
	[WBCAN_ETHTOOL_TX_FRAMES]		= "tx_frames",
	[WBCAN_ETHTOOL_RX_FRAMES]		= "rx_frames",
	[WBCAN_ETHTOOL_FAULT_CANDIDATES]	= "fault_candidates",
	[WBCAN_ETHTOOL_FAULT_INJECTED]	= "fault_injected",
	[WBCAN_ETHTOOL_BUS_ERRORS]		= "bus_errors",
	[WBCAN_ETHTOOL_ARBITRATION_LOST]	= "arbitration_lost",
	[WBCAN_ETHTOOL_RESTART_ATTEMPTS]	= "restart_attempts",
	[WBCAN_ETHTOOL_STOP_ATTEMPTS]	= "stop_attempts",
};

/*
 * 故障平面的注入模式。武装（arm）之后，匹配帧按 fault_after 先跳过、
 * 再连续命中 fault_count 次（判定见 wbcan_should_inject()）。每个取值
 * 的可观测效果：DROP_TX 帧在统计上发送但不到达对端；DROP_RX 回环帧被
 * 吞掉；BIT_FLIP 破坏回环负载的一个位；BUS_OFF 触发控制器 bus-off 迁移；
 * TX_FULL 停止队列并返回 NETDEV_TX_BUSY，给协议栈施加背压（队列唤醒后
 * 同一帧重试）；ARB_LOST / STUFF_ERR 走错误帧与错误统计。
 */
/* 故障模式。取值即 debugfs ABI；不得重新编号。 */
enum wbcan_fault {
	WBCAN_FAULT_NONE	= 0,
	WBCAN_FAULT_DROP_TX	= 1,	/* 帧在被接受后消失 */
	WBCAN_FAULT_DROP_RX	= 2,	/* 帧永远不会到达对端套接字 */
	WBCAN_FAULT_BIT_FLIP	= 3,	/* 破坏一个负载位 */
	WBCAN_FAULT_BUS_OFF	= 4,	/* 控制器离开总线 */
	WBCAN_FAULT_TX_FULL	= 5,	/* 邮箱已满：向协议栈返回 -ENOBUFS */
	WBCAN_FAULT_ARB_LOST	= 6,	/* 仲裁失败，TX 中止 */
	WBCAN_FAULT_STUFF_ERR	= 7,	/* 线上的协议违规 */
	WBCAN_FAULT_MAX
};

/* 与 enum wbcan_fault 逐位对应的 debugfs 文本名：inject 写入按此表做
 * 名字匹配，status 输出按此表打印 armed_fault。文本也是 ABI。 */
static const char *const wbcan_fault_names[] = {
	[WBCAN_FAULT_NONE]	= "none",
	[WBCAN_FAULT_DROP_TX]	= "drop-tx",
	[WBCAN_FAULT_DROP_RX]	= "drop-rx",
	[WBCAN_FAULT_BIT_FLIP]	= "bit-flip",
	[WBCAN_FAULT_BUS_OFF]	= "bus-off",
	[WBCAN_FAULT_TX_FULL]	= "tx-full",
	[WBCAN_FAULT_ARB_LOST]	= "arb-lost",
	[WBCAN_FAULT_STUFF_ERR]	= "stuff-err",
};

/* ------------------------------------------------------------ 模块参数
 *
 * 三个参数都只服务于测试，真实运行不需要任何参数。0600 权限允许 root
 * 在运行时经 sysfs 动态调整；各延迟参数的生效上限见使用点的 1000U 截断。
 */
/* 置 1 强制 alloc_can_err_skb() 返回 NULL：验证错误帧分配失败时
 * can_change_state()/can_bus_off() 的状态迁移仍然完整（bus-off 与错误
 * 警告路径绝不依赖可选错误帧才成立）。 */
static bool fail_error_skb;
module_param(fail_error_skb, bool, 0600);
MODULE_PARM_DESC(fail_error_skb, "fail CAN error SKB allocation for testing");

/* 在 wbcan_set_mode() 序列化 CAN 重启之前注入的测试延迟（毫秒，使用点
 * 截断至 1000）：模拟真实控制器重启耗时，覆盖与重启竞争的路径。 */
static unsigned int test_restart_delay_ms;
module_param(test_restart_delay_ms, uint, 0600);
MODULE_PARM_DESC(test_restart_delay_ms,
		 "test-only delay before serializing CAN restart");

/* 在 wbcan_stop() 的 TX 排空前后各注入一次的测试延迟（毫秒，使用点截断
 * 至 1000）：覆盖停止路径与重启工作线程的竞争窗口。 */
static unsigned int test_stop_delay_ms;
module_param(test_stop_delay_ms, uint, 0600);
MODULE_PARM_DESC(test_stop_delay_ms,
		 "test-only delay around TX drain and stop publication");

/* --------------------------------------------------- 私有数据与统计 */
/*
 * 设备私有数据，紧跟在 struct net_device 之后（netdev_priv() 取回）。
 * 前两个成员构成 CAN 核心契约：can_priv 必须是首成员（CAN 核心按固定
 * 偏移访问状态机与统计），dev 是回指宿主设备的指针。
 *
 * 并发划分：lock 保护故障平面（fault_*、match_*、flip_*）与私有统计
 * （stat_*）；can.state 与 netdev 队列状态由 TX 队列锁与生命周期约定保护
 * （见下方「控制器状态的归属」）；dev->stats 由 lock 与 stats_sync 双重
 * 保护（写入见 wbcan_stats_add()，读取见 wbcan_get_stats64()）。
 */
struct wbcan_priv {
	struct can_priv		can;	/* 必须位于首位：can_priv 契约 */
	struct net_device	*dev;
	struct dentry		*dbg_dir;

	spinlock_t		lock;	/* 保护故障配置与统计 */
	struct u64_stats_sync	stats_sync;	/* 保护 netdev 统计快照 */

	enum wbcan_fault	fault;
	u32			fault_count;	/* 剩余待作用的帧数；0 = 关闭 */
	u32			fault_after;	/* 先跳过这么多帧 */
	canid_t			match_id;	/* 含 CAN_EFF_FLAG */
	bool			match_any;
	u8			flip_byte;
	u8			flip_bit;

	/*
	 * 可观测性。无法计数的故障，
	 * 就是无法在测试中断言的故障。
	 * stat_tx：驱动最终接受的发送帧数（含被 DROP_TX / 错误故障吞掉的帧，
	 * 不含 NETDEV_TX_BUSY 重试帧）；stat_rx：经 netif_rx() 成功投递的
	 * 回环帧数；stat_seen：进入匹配判定的候选帧数；stat_injected：实际
	 * 注入的故障次数；stat_restart_attempts / stat_stop_attempts：重启
	 * 与停止路径的进入次数。全部经 ethtool 与 debugfs status 导出。
	 */
	u64			stat_tx;
	u64			stat_rx;
	u64			stat_injected;
	u64			stat_seen;
	u64			stat_restart_attempts;
	u64			stat_stop_attempts;
};

/*
 * 把一次路径事件累加进 dev->stats 的七个计数位。netdev 统计的读者
 * （RTNL 下的 get_stats64()）与运行中的收发路径共享这些字段，因此写入
 * 必须双保险：lock 与其它 wbcan 私有计数互斥；stats_sync 包围增量，使
 * 读者可用 u64_stats_fetch_begin()/retry() 做无锁无撕裂读取。调用点跨
 * 进程上下文（debugfs/ethtool）与软中断上下文（发送路径），故用 irqsave
 * 变体统一互斥。
 */
static void wbcan_stats_add(struct wbcan_priv *priv, u64 tx_packets,
			    u64 tx_bytes, u64 tx_errors, u64 tx_dropped,
			    u64 rx_packets, u64 rx_bytes, u64 rx_dropped)
{
	struct net_device_stats *stats = &priv->dev->stats;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	u64_stats_update_begin(&priv->stats_sync);
	stats->tx_packets += tx_packets;
	stats->tx_bytes += tx_bytes;
	stats->tx_errors += tx_errors;
	stats->tx_dropped += tx_dropped;
	stats->rx_packets += rx_packets;
	stats->rx_bytes += rx_bytes;
	stats->rx_dropped += rx_dropped;
	u64_stats_update_end(&priv->stats_sync);
	spin_unlock_irqrestore(&priv->lock, flags);
}

/*
 * ndo_get_stats64() 回调：向用户态（/proc/net/dev、ip -s link）导出
 * dev->stats 快照。调用方已持 RTNL，无需再
 * 加锁；读端用 u64_stats_fetch_begin()/u64_stats_fetch_retry() 的 seqlock
 * 循环与 wbcan_stats_add() 的更新端配对，保证 64 位计数不被撕裂。
 */
static void wbcan_get_stats64(struct net_device *dev,
			      struct rtnl_link_stats64 *stats)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&priv->stats_sync);
		stats->tx_packets = dev->stats.tx_packets;
		stats->tx_bytes = dev->stats.tx_bytes;
		stats->tx_errors = dev->stats.tx_errors;
		stats->tx_dropped = dev->stats.tx_dropped;
		stats->rx_packets = dev->stats.rx_packets;
		stats->rx_bytes = dev->stats.rx_bytes;
		stats->rx_dropped = dev->stats.rx_dropped;
	} while (u64_stats_fetch_retry(&priv->stats_sync, start));
}

/*
 * debugfs status 文件的一次性快照：所有字段在 lock 内复制，随后在锁外
 * 格式化。这样 kernel/wbcan/LATENCY.md「配置档」的 status-readers 可以
 * 高频轮询，而格式化不会拉长 TX 关键路径。state 与 queue_stopped 用
 * READ_ONCE() / netif_queue_stopped() 独立读取，可能描述相邻两个瞬间
 * （它们是诊断遥测，不是控制权——见下方归属注释）。
 */
struct wbcan_status_snapshot {
	enum can_state		state;
	bool			queue_stopped;
	enum wbcan_fault	fault;
	u32			fault_count;
	u32			fault_after;
	canid_t			match_id;
	bool			match_any;
	u64			stat_tx;
	u64			stat_rx;
	u64			stat_injected;
	u64			stat_seen;
	u64			stat_restart_attempts;
	u64			stat_stop_attempts;
	u32			bus_errors;
};

/*
 * 控制器状态的归属遵循 netdev/CAN 核心的生命周期，
 * 而非私有的故障锁：
 *
 * - ndo_start_xmit() 及其注入的错误迁移由单个 netdev TX 队列串行化；
 * - bus-off 在发布终态之前先停止该队列；
 * - do_set_mode() 仅在 CAN 核心恢复保持队列停止期间运行；
 * - ndo_open()/ndo_stop() 在 RTNL 下运行，且 ndo_stop()
 *   在关闭 CAN 设备之前先禁用 TX。
 * - netif_tx_lock_bh() 显式取得 TX 队列锁：open/stop/set_mode 借此把
 *   can.state 的写与并发 ndo_start_xmit() 串行化（start_xmit 天然在该
 *   锁下运行）；
 * - netif_tx_disable() 不止置停队列位，还会逐个队列取锁等待在途
 *   start_xmit() 返回后才放开；
 * - can.state 因此一律经 READ_ONCE()/WRITE_ONCE() 访问，把锁纪律写成
 *   文档化标记。
 *
 * debugfs 在私有锁内为故障平面拍快照，并在不持有 netdev TX 锁的
 * 情况下读取独立发布的 CAN 状态/队列位。格式化在私有锁之外进行，
 * 因此状态观测不会拉长 TX 关键路径。状态与队列的取值可能描述
 * 相邻的两个瞬间；它们是诊断遥测，不是控制权。
 */

/* ------------------------------------------------------------------ 辅助函数 */

/* 判定这一帧是否命中故障，命中则消耗一次机会。
 * 在持有锁的情况下调用。
 * wbcan_match_key() 本身只做 ID 归一化：扩展帧取 CAN_EFF_FLAG | (id &
 * CAN_EFF_MASK)，标准帧取 id & CAN_SFF_MASK，使帧内 can_id 与 debugfs
 * 写入的 match_id 按同一种键比较；CAN_RTR_FLAG 不参与匹配键。
 */
static canid_t wbcan_match_key(canid_t id)
{
	if (id & CAN_EFF_FLAG)
		return CAN_EFF_FLAG | (id & CAN_EFF_MASK);
	return id & CAN_SFF_MASK;
}

/*
 * 故障命中判定（调用者必须持 priv->lock）。判定顺序：
 * - 未武装（WBCAN_FAULT_NONE）或次数耗尽（fault_count == 0）→ 不注入；
 * - 未设 match_any 且帧 ID 与 match_id 不匹配 → 不注入；
 * - bit-flip 对远程帧（CAN_RTR_FLAG）或 flip_byte 越过负载长度的帧
 *   → 不注入（无位可翻）；
 * - 其余帧计入 stat_seen（候选），fault_after 未归零时只跳过不注入；
 * - 最终命中者消耗一次 fault_count、递增 stat_injected，并经输出参数
 *   带回故障模式与翻转位置。
 * struct can_frame 直接映射在 skb->data 上（SocketCAN 布局，CAN_MTU 共
 * 16 字节：can_id 4 + len 1 + flags 1 + 保留 2 + data[8]）。
 */
static bool wbcan_should_inject(struct wbcan_priv *priv, struct sk_buff *skb,
				enum wbcan_fault *fault, u8 *flip_byte,
				u8 *flip_bit)
{
	struct can_frame *cf = (struct can_frame *)skb->data;

	if (priv->fault == WBCAN_FAULT_NONE || priv->fault_count == 0)
		return false;

	if (!priv->match_any && wbcan_match_key(cf->can_id) != priv->match_id)
		return false;
	if (priv->fault == WBCAN_FAULT_BIT_FLIP &&
	    ((cf->can_id & CAN_RTR_FLAG) ||
	     priv->flip_byte >= can_skb_get_data_len(skb)))
		return false;

	priv->stat_seen++;

	if (priv->fault_after > 0) {
		priv->fault_after--;
		return false;
	}

	priv->fault_count--;
	priv->stat_injected++;
	*fault = priv->fault;
	*flip_byte = priv->flip_byte;
	*flip_bit = priv->flip_bit;
	return true;
}

/* 向用户态上抛一个 CAN 错误帧，并推进控制器状态。
 *
 * 这是用户态垫片做不到的部分。can_change_state() 更新
 * can_priv 状态与 berr 计数器，而错误帧正是 candump
 * 渲染为 "ERRORFRAME"、固件错误处理程序所读取的内容。
 * alloc_can_err_skb() 预置 can_id = CAN_ERR_FLAG 与 len = CAN_ERR_DLC
 * （8 字节）；fail_error_skb 模块参数可强制分配失败，以验证状态迁移
 * 绝不依赖可选错误帧——错误帧是诊断载体，不是控制权。各分支只补充
 * can_id 标志与 data[] 偏移（布局见 include/uapi/linux/can/error.h）：
 * data[0] 仲裁丢失位位置、data[1] 控制器状态、data[2..3] 协议错误细节。
 * CAN 状态阈值（include/uapi/linux/can/netlink.h）：TEC/REC < 96 为
 * active，< 128 为 warning，< 256 为 passive，≥ 256 为 bus-off——本
 * 驱动只驱动「状态迁移」这一抽象，不建模真实错误计数器的演进。
 */
static void wbcan_emit_error(struct net_device *dev, enum wbcan_fault fault)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	struct can_frame *cf = NULL;
	struct sk_buff *skb;
	enum can_state tx_state = READ_ONCE(priv->can.state);
	enum can_state rx_state = tx_state;
	unsigned long flags;

	skb = READ_ONCE(fail_error_skb) ? NULL : alloc_can_err_skb(dev, &cf);

	switch (fault) {
	case WBCAN_FAULT_BUS_OFF:
		/*
		 * bus-off 在驱动重启之前是终态。若设置了 restart-ms，
		 * CAN 核心会处理重启定时器，这正是 FW19 所验证的。
		 * 状态恢复绝不能依赖分配可选错误帧。
		 */
		netif_stop_queue(dev);
		tx_state = CAN_STATE_BUS_OFF;
		rx_state = CAN_STATE_BUS_OFF;
		if (READ_ONCE(priv->can.state) != CAN_STATE_BUS_OFF)
			can_change_state(dev, cf, tx_state, rx_state);
		else if (cf)
			cf->can_id |= CAN_ERR_BUSOFF;
		can_bus_off(dev);
		/* can_bus_off()：关闭载波（用户态链路 down 通知，适配器据此进入
		 * BUS_OFF），并在 restart-ms 配置时调度 CAN 核心的重启定时器。 */
		break;

	case WBCAN_FAULT_ARB_LOST:
		spin_lock_irqsave(&priv->lock, flags);
		priv->can.can_stats.arbitration_lost++;
		spin_unlock_irqrestore(&priv->lock, flags);
		if (!cf)
			break;
		cf->can_id |= CAN_ERR_LOSTARB;
		/*
		 * 仲裁失败所处的位位置。0 表示未指定——在这里这是实话：
		 * 我们并未建模真实的位时间线。
		 */
		cf->data[0] = 0;
		break;

	case WBCAN_FAULT_STUFF_ERR:
		/*
		 * 这是一个受限的协议错误模型：每个注入帧产生一次警告，
		 * 下一帧无故障帧即恢复到 active。
		 * 我们不假装建模 TEC/REC 的演进过程。
		 */
		spin_lock_irqsave(&priv->lock, flags);
		priv->can.can_stats.bus_error++;
		spin_unlock_irqrestore(&priv->lock, flags);
		tx_state = CAN_STATE_ERROR_WARNING;
		/* 当计算出的状态未变化时，can_change_state() 会发出警告。 */
		if (max(tx_state, rx_state) != READ_ONCE(priv->can.state))
			can_change_state(dev, cf, tx_state, rx_state);
		if (!cf)
			break;
		cf->can_id |= CAN_ERR_PROT;
		cf->data[2] = CAN_ERR_PROT_STUFF;
		break;

	/* 其余故障（当前只有 TX_FULL）只产生通用控制错误帧：
	 * CAN_ERR_CRTL 标志 + data[1] = CAN_ERR_CRTL_UNSPEC。 */
	default:
		if (!cf)
			return;
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] = CAN_ERR_CRTL_UNSPEC;
		break;
	}

	/* 错误帧投递失败（NET_RX_DROP）计入 rx_dropped；成功投递不计入
	 * rx_packets——错误帧不是数据帧，与 tx/rx 数据计数分开。 */
	if (skb && netif_rx(skb) != NET_RX_SUCCESS)
		wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
}

/* ------------------------------------------------------------ netdev 操作 */

/*
 * ndo_open()：`ip link set wbcan0 up` 进入，持 RTNL。open_candev() 校验
 * 位时序已定义（CAN FD 设备还校验数据相位）并在设备曾于 bus-off 状态被
 * 关闭时重新打开载波；随后在 TX 队列锁内把状态置为 CAN_STATE_ERROR_ACTIVE
 * 再放行队列——顺序保证第一个入队帧看到的是 active 而非 STOPPED（初始化
 * 时的占位态）。open_candev() 失败时接口保持 down。
 */
static int wbcan_open(struct net_device *dev)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	int err;

	err = open_candev(dev);
	if (err)
		return err;
	netif_tx_lock_bh(dev);
	WRITE_ONCE(priv->can.state, CAN_STATE_ERROR_ACTIVE);
	netif_tx_unlock_bh(dev);
	netif_start_queue(dev);
	return 0;
}

/*
 * ndo_stop()：`ip link set wbcan0 down` 进入，持 RTNL。顺序契约：
 * 先 netif_tx_disable() 停止接收新帧并等待在途 start_xmit() 返回，再在
 * 队列锁内写 STOPPED 状态，随后 close_candev() 取消可能已排队的重启
 * 定时器（并清空 echo 队列），最后再 netif_tx_disable() 一次——防御
 * 重启工作线程在 STOPPED 发布前提交 ACTIVE（见 wbcan_set_mode()）。
 * test_stop_delay_ms 在排空前后各注入一次延迟（使用点截断至 1000 ms），
 * 用于测试与停止竞争的路径。
 */
static int wbcan_stop(struct net_device *dev)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	unsigned int delay_ms;
	unsigned long flags;

	/* 停止新提交，并等待在途的 start_xmit() 结束。 */
	netif_tx_disable(dev);
	spin_lock_irqsave(&priv->lock, flags);
	priv->stat_stop_attempts++;
	spin_unlock_irqrestore(&priv->lock, flags);
	delay_ms = min(READ_ONCE(test_stop_delay_ms), 1000U);
	if (delay_ms)
		msleep(delay_ms);
	netif_tx_lock_bh(dev);
	netif_stop_queue(dev);
	WRITE_ONCE(priv->can.state, CAN_STATE_STOPPED);
	netif_tx_unlock_bh(dev);
	if (delay_ms)
		msleep(delay_ms);
	/* 在 STOPPED 之前排队的重启工作线程必须被取消。 */
	close_candev(dev);
	/* 工作线程可能在 STOPPED 发布之前已提交 ACTIVE。 */
	netif_tx_disable(dev);
	return 0;
}

/*
 * ndo_start_xmit()：SocketCAN 发送入口，所有发送帧（含回环帧的生成）
 * 都经由这里。它天然在 TX 队列锁（netif_tx_lock_bh 语义，多核串行）下
 * 运行，与 open/stop/set_mode 的显式取锁互斥；函数内的私有锁只保护
 * 故障平面与统计。
 *
 * 返回码契约（include/linux/netdevice.h）：
 * - NETDEV_TX_OK（0）：驱动已消费 skb——无论送达、丢弃还是注入故障，
 *   栈不再重试；
 * - NETDEV_TX_BUSY（0x10）：TX 路径繁忙，协议栈保留 skb 稍后重试。
 *   驱动必须先 netif_stop_queue() 再返回 BUSY，否则栈会热循环；队列
 *   唤醒后同一帧重新进入本函数。
 *
 * 帧生命周期：can_dev_dropped_skb() 有效性校验 → 错误帧拦截 → 终态
 * 防御 → 故障判定 → 按故障类型接受/丢弃/报错 → 按 skb->pkt_type ==
 * PACKET_LOOPBACK 决定是否回环。IFF_ECHO 置位时，CAN 核心（af_can 的
 * can_send()）把请求本地回环的发送帧标记为 PACKET_LOOPBACK；驱动据此
 * 只克隆需要回环的帧，不克隆纯广播帧。
 */
static netdev_tx_t wbcan_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	struct can_frame *cf = (struct can_frame *)skb->data;
	struct sk_buff *rx_skb;
	enum wbcan_fault fault = WBCAN_FAULT_NONE;
	u8 flip_byte = 0;
	u8 flip_bit = 0;
	bool loop;
	unsigned int len;
	unsigned long flags;
	enum can_state state;

	/* 无效帧直接丢弃：can_dev_dropped_skb() 在 LISTENONLY 模式丢弃一切
	 * 发送帧，否则委托 can_dropped_invalid_skb() 按 dev->mtu 校验帧长
	 * （Classic CAN 只收 CAN_MTU），无效时释放 skb 并递增 tx_dropped。 */
	if (can_dev_dropped_skb(dev, skb))
		return NETDEV_TX_OK;

	/* 回环我们自己的错误帧会形成循环。 */
	/* 错误帧（CAN_ERR_FLAG）只出不进：若被回环，它会作为新发送再次进入
	 * 本函数形成无限自环。kfree_skb() 走丢弃路径（区别于成功路径的
	 * consume_skb()）。 */
	if (cf->can_id & CAN_ERR_FLAG) {
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	state = READ_ONCE(priv->can.state);
	if (state == CAN_STATE_BUS_OFF || state == CAN_STATE_STOPPED ||
	    state == CAN_STATE_SLEEPING) {
		/*
		 * 队列生命周期应当让这些状态进不了 start_xmit()。
		 * 若未来的调用方破坏该边界，则应消费该帧，
		 * 而不是在控制器的终态下接受流量。
		 */
		netif_stop_queue(dev);
		wbcan_stats_add(priv, 0, 0, 0, 1, 0, 0, 0);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	/* 故障判定与计数在私有锁内原子完成，注入参数经输出参数带出。 */
	spin_lock_irqsave(&priv->lock, flags);
	wbcan_should_inject(priv, skb, &fault, &flip_byte, &flip_bit);
	spin_unlock_irqrestore(&priv->lock, flags);
	/* 帧负载长度：can_skb_get_data_len() 对 RTR 帧返回 0（RTR 帧无数据段）。 */
	len = can_skb_get_data_len(skb);

	switch (fault) {
	case WBCAN_FAULT_TX_FULL:
		/*
		 * 邮箱已满。停止队列并返回 BUSY，
		 * 是真实驱动施加背压的方式；
		 * 协议栈会在我们唤醒它之后重试。
		 */
		netif_stop_queue(dev);
		wbcan_emit_error(dev, fault);
		/*
		 * 立即唤醒：我们建模的是瞬时满的状态，
		 * 不是卡死。没有这一步测试会挂起。
		 */
		netif_wake_queue(dev);
		return NETDEV_TX_BUSY;

	default:
		break;
	}

	/* 该帧现在已被接受，协议栈不会再重试它。 */
	/* 软件发送时间戳：套接字请求 SOF_TIMESTAMPING_TX_SOFTWARE 时在此刻
	 * 完成（skb_tx_timestamp() 见 skbuff.h，先克隆 PHY 时间戳再对
	 * SKBTX_SW_TSTAMP 生成完成时间戳）。BUSY 重试帧尚未走到这里，重试
	 * 通过后才打时间戳。 */
	skb_tx_timestamp(skb);

	switch (fault) {
	case WBCAN_FAULT_BUS_OFF:
	case WBCAN_FAULT_ARB_LOST:
	case WBCAN_FAULT_STUFF_ERR:
		/* 三类线上错误故障：帧被控制器「接受」但线上失败——统计
		 * stat_tx 与 tx_errors，向套接字发错误帧，数据帧本身不投递
		 * （kfree_skb 丢弃路径）。 */
		spin_lock_irqsave(&priv->lock, flags);
		priv->stat_tx++;
		spin_unlock_irqrestore(&priv->lock, flags);
		wbcan_emit_error(dev, fault);
		wbcan_stats_add(priv, 0, 0, 1, 0, 0, 0, 0);
		kfree_skb(skb);
		return NETDEV_TX_OK;

	case WBCAN_FAULT_DROP_TX:
		/*
		 * 已接受、已计数、永不送达。对固件而言这是最阴险的
		 * 故障：没有错误，也没有帧。
		 */
		/* 统计按成功发送计（tx_packets/tx_bytes 照常累加），但帧不会
		 * 出现在任何套接字上——「静默丢帧」故障模型。 */
		spin_lock_irqsave(&priv->lock, flags);
		priv->stat_tx++;
		spin_unlock_irqrestore(&priv->lock, flags);
		wbcan_stats_add(priv, 1, len, 0, 0, 0, 0, 0);
		kfree_skb(skb);
		return NETDEV_TX_OK;

	default:
		break;
	}

	/* 统计被驱动接受的帧；BUSY 重试不算一帧。 */
	/* 能走到这里的只剩 WBCAN_FAULT_NONE 与 WBCAN_FAULT_BIT_FLIP：帧被
	 * 接受，继续正常路径（下方按需回环）。 */
	spin_lock_irqsave(&priv->lock, flags);
	priv->stat_tx++;
	spin_unlock_irqrestore(&priv->lock, flags);
	/* stuff-err 的受限恢复模型：无故障帧顺利通过时，从 ERROR_WARNING
	 * 一步回到 ERROR_ACTIVE（cf = NULL：只迁移状态，不发错误帧）。 */
	if (fault == WBCAN_FAULT_NONE &&
	    READ_ONCE(priv->can.state) == CAN_STATE_ERROR_WARNING)
		can_change_state(dev, NULL, CAN_STATE_ERROR_ACTIVE,
				 CAN_STATE_ERROR_ACTIVE);

	wbcan_stats_add(priv, 1, len, 0, 0, 0, 0, 0);
	/* 回环判定：IFF_ECHO 置位时 af_can 的 can_send() 把请求本地回环的帧
	 * 标记为 PACKET_LOOPBACK。无回环请求的帧只存在于「总线」上，本地
	 * 套接字不可见，consume_skb() 按成功路径释放。 */
	loop = skb->pkt_type == PACKET_LOOPBACK;
	if (!loop) {
		consume_skb(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * 保留原始套接字，使 CAN_RAW_RECV_OWN_MSGS 与接收确认标志
	 * 维持标准的 SocketCAN 语义。位翻转需要私有数据副本，
	 * 因为包抓取点可能持有共享克隆。
	 * can_create_echo_skb() = skb_clone() + can_skb_set_owner(原 skb->sk)
	 * + consume_skb(原帧)：克隆体携带原套接字引用，回环投递时 CAN 核心
	 * 据此落实 CAN_RAW_RECV_OWN_MSGS、接收确认标志与 msghdr 归因。
	 * bit-flip 必须 skb_copy()（深拷贝数据区）而非克隆：负载位要翻转，
	 * 且 tcpdump 等包抓取点可能共享该 skb。
	 */
	if (fault == WBCAN_FAULT_BIT_FLIP) {
		rx_skb = skb_copy(skb, GFP_ATOMIC);
		if (rx_skb)
			can_skb_set_owner(rx_skb, skb->sk);
		consume_skb(skb);
	} else {
		rx_skb = can_create_echo_skb(skb);
	}
	/* echo skb 分配失败：帧已发送但无法回环，计入 rx_dropped。 */
	if (!rx_skb) {
		wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
		return NETDEV_TX_OK;
	}

	/* 翻转一个负载位：flip_byte/flip_bit 在武装时已被限定在 0..7，
	 * 且 should_inject() 保证 flip_byte 不越过 DLC。 */
	if (fault == WBCAN_FAULT_BIT_FLIP) {
		struct can_frame *rcf = (struct can_frame *)rx_skb->data;

		if (flip_byte < rcf->len) {
			rcf->data[flip_byte] ^= (1u << flip_bit);
			netdev_dbg(dev, "flipped byte %u bit %u of id 0x%x\n",
				   flip_byte, flip_bit, rcf->can_id);
		}
	}

	/* drop-rx：回环帧在交付前被吞掉——发送方自己与所有监听者都收不到。 */
	if (fault == WBCAN_FAULT_DROP_RX) {
		kfree_skb(rx_skb);
		wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
	} else {
		/* 标准回环投递（vcan 同款设置）：dev 指向本设备，
		 * CHECKSUM_UNNECESSARY 表示无 IP 校验和，PACKET_BROADCAST 表示
		 * CAN 帧按广播语义分发给所有匹配的本地套接字。 */
		rx_skb->dev = dev;
		rx_skb->ip_summed = CHECKSUM_UNNECESSARY;
		rx_skb->pkt_type = PACKET_BROADCAST;
		if (netif_rx(rx_skb) == NET_RX_SUCCESS) {
			wbcan_stats_add(priv, 0, 0, 0, 0, 1, len, 0);

			spin_lock_irqsave(&priv->lock, flags);
			priv->stat_rx++;
			spin_unlock_irqrestore(&priv->lock, flags);
		} else {
			wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
		}
	}

	return NETDEV_TX_OK;
}

/* 由 CAN 核心的重启定时器调用，也由
 * `ip link set can0 type can restart` 调用。
 * FW19 的 bus-off 恢复测试驱动此路径。
 * 进入时 CAN 核心（can_restart_now()）已同步 TX 队列并保持队列停止，
 * 因此恢复成功前不会放行新流量；非 BUS_OFF 状态返回 -EBUSY（重启只对
 * bus-off 有意义），未实现的模式返回 -EOPNOTSUPP。
 * test_restart_delay_ms 模拟真实控制器的重启串行化延迟（使用点截断至
 * 1000 ms）；重启同时清空故障平面，避免恢复测试反复抖动。
 */
static int wbcan_set_mode(struct net_device *dev, enum can_mode mode)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	unsigned int delay_ms;
	unsigned long flags;

	switch (mode) {
	case CAN_MODE_START:
		spin_lock_irqsave(&priv->lock, flags);
		priv->stat_restart_attempts++;
		spin_unlock_irqrestore(&priv->lock, flags);

		delay_ms = min(READ_ONCE(test_restart_delay_ms), 1000U);
		if (delay_ms)
			msleep(delay_ms);

		/* CAN 核心恢复拥有此回调，并保持 TX 停止。 */
		netif_tx_lock_bh(dev);
		if (READ_ONCE(priv->can.state) != CAN_STATE_BUS_OFF) {
			netif_tx_unlock_bh(dev);
			return -EBUSY;
		}

		spin_lock_irqsave(&priv->lock, flags);
		/*
		 * 重启时清除已武装的故障。若让其保持武装，
		 * 恢复测试会因测试并未要求的缘由而反复抖动。
		 */
		priv->fault = WBCAN_FAULT_NONE;
		priv->fault_count = 0;
		spin_unlock_irqrestore(&priv->lock, flags);

		WRITE_ONCE(priv->can.state, CAN_STATE_ERROR_ACTIVE);
		netif_wake_queue(dev);
		netif_tx_unlock_bh(dev);
		netdev_info(dev, "restarted, fault plane cleared\n");
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * ndo_change_mtu()：只接受 CAN_MTU（16 字节 struct can_frame）。拒绝
 * CANFD_MTU / CANXL_MTU 意味着本设备永远是 Classic CAN——与 Wire V1
 * 契约（docs/architecture/mcu-wire-v1.md「传输边界」：Classic CAN 2.0
 * 数据帧、DLC 恰为 8）保持一致。接口已 up（IFF_UP）时返回 -EBUSY，
 * 其余取值 -EINVAL；成功时在 RTNL 下更新 dev->mtu。
 */
static int wbcan_change_mtu(struct net_device *dev, int new_mtu)
{
	if (dev->flags & IFF_UP)
		return -EBUSY;
	if (new_mtu != CAN_MTU)
		return -EINVAL;

	WRITE_ONCE(dev->mtu, new_mtu);
	return 0;
}

/*
 * net_device_ops：各回调的调用上下文——open/stop/change_mtu 持 RTNL；
 * start_xmit 持 TX 队列锁（软中断上下文）；get_stats64 持 RTNL 读统计。
 * 未实现的其余回调保持 NULL，由 netdev 核心回落默认实现。
 */
static const struct net_device_ops wbcan_netdev_ops = {
	.ndo_open	= wbcan_open,
	.ndo_stop	= wbcan_stop,
	.ndo_start_xmit	= wbcan_start_xmit,
	.ndo_change_mtu	= wbcan_change_mtu,
	.ndo_get_stats64	= wbcan_get_stats64,
};

/* ------------------------------------------------------------ ethtool 操作 */
/*
 * ethtool get_sset_count()：ETH_SS_STATS 时报告私有统计条数
 * （WBCAN_ETHTOOL_STAT_COUNT），其余字符串集返回 -EOPNOTSUPP。
 */
static int wbcan_get_sset_count(struct net_device *dev, int stringset)
{
	if (stringset == ETH_SS_STATS)
		return WBCAN_ETHTOOL_STAT_COUNT;
	return -EOPNOTSUPP;
}

/*
 * ethtool get_strings()：把统计名逐条拷贝进调用方缓冲区，每条至多
 * ETH_GSTRING_LEN（32）字节。输出即观测 ABI，改名前须确认消费方。
 */
static void wbcan_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	unsigned int index;

	if (stringset != ETH_SS_STATS)
		return;
	for (index = 0; index < WBCAN_ETHTOOL_STAT_COUNT; index++)
		strscpy(data + index * ETH_GSTRING_LEN,
			wbcan_ethtool_stat_names[index], ETH_GSTRING_LEN);
}

/*
 * ethtool get_ethtool_stats()：在私有锁内一次快照 8 个统计值，保证单次
 * ethtool 查询内部一致。bus_errors 与 arbitration_lost 存在 CAN 核心
 * 维护的 can_stats 里，由 wbcan_emit_error() 在注入故障时递增；其余
 * 来自本驱动私有计数。与 debugfs status 的统计字段一一对应。
 */
static void wbcan_get_ethtool_stats(struct net_device *dev,
				    struct ethtool_stats *stats, u64 *data)
{
	struct wbcan_priv *priv = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	data[WBCAN_ETHTOOL_TX_FRAMES] = priv->stat_tx;
	data[WBCAN_ETHTOOL_RX_FRAMES] = priv->stat_rx;
	data[WBCAN_ETHTOOL_FAULT_CANDIDATES] = priv->stat_seen;
	data[WBCAN_ETHTOOL_FAULT_INJECTED] = priv->stat_injected;
	data[WBCAN_ETHTOOL_BUS_ERRORS] = priv->can.can_stats.bus_error;
	data[WBCAN_ETHTOOL_ARBITRATION_LOST] =
		priv->can.can_stats.arbitration_lost;
	data[WBCAN_ETHTOOL_RESTART_ATTEMPTS] = priv->stat_restart_attempts;
	data[WBCAN_ETHTOOL_STOP_ATTEMPTS] = priv->stat_stop_attempts;
	spin_unlock_irqrestore(&priv->lock, flags);
}

/*
 * ethtool_ops：get_ts_info 用内核通用实现 ethtool_op_get_ts_info()，
 * 报告纯软件时间戳能力（无 PTP 硬件时钟）。host-can-transport-v1.md
 * 依赖的 SO_TIMESTAMPNS 接收时间戳由套接字层生成，与这里声明的软件
 * 时间戳能力一致；TX 侧软件时间戳见 start_xmit() 的 skb_tx_timestamp()。
 */
static const struct ethtool_ops wbcan_ethtool_ops = {
	.get_ts_info		= ethtool_op_get_ts_info,
	.get_strings		= wbcan_get_strings,
	.get_ethtool_stats	= wbcan_get_ethtool_stats,
	.get_sset_count		= wbcan_get_sset_count,
};

/* ------------------------------------------------------------ debugfs ABI
 *
 * echo "<mode> <count> [after] [id] [byte] [bit]" > /sys/kernel/debug/wbcan/<dev>/inject
 *
 *   武装 3 帧 drop-tx：                echo "drop-tx 3" > inject
 *   翻转字节 0 的位 2，第 4 帧起生效：echo "bit-flip 1 3 any 0 2" > inject
 *   仅对标准 ID 0x123 触发 bus-off：  echo "bus-off 1 0 s:123" > inject
 *   丢弃扩展 ID 0x123：               echo "drop-tx 1 0 e:123" > inject
 *
 * 采用文本而非 ioctl：CI 中的 shell 脚本需要驱动它，
 * 文本 ABI 只需一行 bash，而无需辅助二进制。
 * status 文件（0444 只读）输出状态与统计快照；inject 文件（0200 仅
 * root 可写）武装故障。二者一起支撑 kernel/wbcan/LATENCY.md「配置档」
 * 的 status-readers 轮询与 test_wbcan.sh 的故障套件。
 */

/* ------------------------------------------------------- debugfs 处理函数 */
/*
 * 解析 inject 的 match ID 参数（十六进制）：
 * - "any" 或 "ffff"：不按 ID 过滤（match_any = true）；
 * - "s:<id>"：标准 11 位 ID，id > CAN_SFF_MASK 报 -ERANGE；
 * - "e:<id>"：扩展 29 位 ID，id > CAN_EFF_MASK 报 -ERANGE；
 * - 无前缀：id ≤ CAN_SFF_MASK 视为标准帧，更大（≤ CAN_EFF_MASK）视为
 *   扩展帧。
 * 输出的 match_id 仅扩展帧带 CAN_EFF_FLAG 标记（与帧内 can_id 的高位
 * 标志一致），使两者经 wbcan_match_key() 归一化后可直接比较；成功返回
 * 0，失败返回负错误码。
 */
static int wbcan_parse_match_id(const char *value, bool *match_any,
				canid_t *match_id)
{
	const char *number = value;
	bool extended = false;
	u32 id;

	if (sysfs_streq(value, "any") || sysfs_streq(value, "ffff")) {
		*match_any = true;
		*match_id = 0;
		return 0;
	}

	if (!strncmp(value, "s:", 2)) {
		number += 2;
	} else if (!strncmp(value, "e:", 2)) {
		number += 2;
		extended = true;
	}
	if (kstrtou32(number, 16, &id))
		return -EINVAL;
	if (!strncmp(value, "s:", 2) && id > CAN_SFF_MASK)
		return -ERANGE;
	if (!strncmp(value, "e:", 2) && id > CAN_EFF_MASK)
		return -ERANGE;
	if (value == number && id > CAN_SFF_MASK) {
		if (id > CAN_EFF_MASK)
			return -ERANGE;
		extended = true;
	}

	*match_any = false;
	*match_id = id | (extended ? CAN_EFF_FLAG : 0);
	return 0;
}

/*
 * debugfs inject 文件写处理（进程上下文，可睡眠）。格式：
 *   <mode> <count> [after] [id] [byte] [bit]
 * count/after/byte/bit 为十进制；id 为十六进制（见 wbcan_parse_match_id）。
 * "none" 只接受可选的 0 计数，用于清除武装。buf[96] 上界拒绝更长的
 * 命令，保证 buf[len] = '\0' 终止符不越界；copy_from_user 失败返回
 * -EFAULT。apply 段在私有锁内整体替换故障平面并清零 stat_seen，使武装
 * 动作对发送路径原子可见。成功返回写入字节数 len，失败返回负错误码。
 */
static ssize_t wbcan_inject_write(struct file *file, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct wbcan_priv *priv = file->private_data;
	char buf[96], **argv;
	canid_t match_id = 0;
	bool match_any = true;
	u32 count = 0, after = 0, byte = 0, bit = 0;
	unsigned long flags;
	int argc, i, err = 0, fault = -1;

	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	/* 按空白拆词（支持引号分组）；GFP_KERNEL 因为 debugfs 写运行在
	 * 进程上下文。 */
	argv = argv_split(GFP_KERNEL, buf, &argc);
	if (!argv)
		return -ENOMEM;
	if (argc < 1) {
		err = -EINVAL;
		goto out;
	}

	/* 按 wbcan_fault_names[] 匹配模式名；未知名字拒绝并告警。 */
	for (i = 0; i < WBCAN_FAULT_MAX; i++) {
		if (wbcan_fault_names[i] && sysfs_streq(argv[0], wbcan_fault_names[i])) {
			fault = i;
			break;
		}
	}
	if (fault < 0) {
		pr_warn("unknown fault mode '%s'\n", argv[0]);
		err = -EINVAL;
		goto out;
	}

	/* "none" 是清除命令：至多带一个 0 计数，直接跳到 apply。 */
	if (fault == WBCAN_FAULT_NONE) {
		if (argc > 2 || (argc == 2 && (kstrtou32(argv[1], 10, &count) || count))) {
			err = -EINVAL;
			goto out;
		}
		goto apply;
	}
	/* bit-flip 需要完整的 6 个参数（含 byte/bit）；其余故障 2..4 个；
	 * count 必须非零——0 计数的故障等同于未武装。 */
	if ((fault == WBCAN_FAULT_BIT_FLIP && argc != 6) ||
	    (fault != WBCAN_FAULT_BIT_FLIP && (argc < 2 || argc > 4)) ||
	    kstrtou32(argv[1], 10, &count) || count == 0) {
		err = -EINVAL;
		goto out;
	}
	if (argc >= 3 && kstrtou32(argv[2], 10, &after)) {
		err = -EINVAL;
		goto out;
	}
	if (argc >= 4 && wbcan_parse_match_id(argv[3], &match_any, &match_id)) {
		err = -EINVAL;
		goto out;
	}
	if (fault == WBCAN_FAULT_BIT_FLIP &&
	    (kstrtou32(argv[4], 10, &byte) || byte > 7 ||
	     kstrtou32(argv[5], 10, &bit) || bit > 7)) {
		err = -EINVAL;
		goto out;
	}

apply:
	/* 原子武装：故障平面字段与 stat_seen 清零在锁内一次性提交，与发送
	 * 路径的加锁读取互斥。 */
	spin_lock_irqsave(&priv->lock, flags);
	priv->fault       = fault;
	priv->fault_count = count;
	priv->fault_after = after;
	priv->match_id    = match_id;
	priv->match_any   = match_any;
	priv->flip_byte   = (u8)byte;
	priv->flip_bit    = (u8)bit;
	priv->stat_seen   = 0;
	spin_unlock_irqrestore(&priv->lock, flags);

	netdev_info(priv->dev, "armed %s count=%u after=%u match=%s0x%x\n",
		    wbcan_fault_names[fault], count, after,
		    match_any ? "any/" : (match_id & CAN_EFF_FLAG) ? "e:" : "s:",
		    match_id & CAN_EFF_MASK);

out:
	argv_free(argv);
	return err ? err : len;
}

/*
 * debugfs status 的 seq_file 输出：锁内拍快照、锁外格式化（见
 * struct wbcan_status_snapshot）。state 手动映射 CAN_STATE_* 字符串
 * （等价于 can_get_state_str()）；match_id 按 e:/s: 前缀还原 inject 的
 * 解析语法；bus_errors 来自 CAN 核心 can_stats。输出是诊断遥测而非
 * 控制权——快照各字段可能来自相邻瞬间。
 */
static int wbcan_status_show(struct seq_file *s, void *unused)
{
	struct wbcan_priv *priv = s->private;
	struct wbcan_status_snapshot snapshot;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	snapshot.state = READ_ONCE(priv->can.state);
	snapshot.queue_stopped = netif_queue_stopped(priv->dev);
	snapshot.fault = priv->fault;
	snapshot.fault_count = priv->fault_count;
	snapshot.fault_after = priv->fault_after;
	snapshot.match_id = priv->match_id;
	snapshot.match_any = priv->match_any;
	snapshot.stat_tx = priv->stat_tx;
	snapshot.stat_rx = priv->stat_rx;
	snapshot.stat_injected = priv->stat_injected;
	snapshot.stat_seen = priv->stat_seen;
	snapshot.stat_restart_attempts = priv->stat_restart_attempts;
	snapshot.stat_stop_attempts = priv->stat_stop_attempts;
	snapshot.bus_errors = priv->can.can_stats.bus_error;
	spin_unlock_irqrestore(&priv->lock, flags);

	/* CAN_STATE_* 的手动字符串映射（阈值语义见 include/uapi/linux/can/
	 * netlink.h：< 96 / < 128 / < 256 / ≥ 256 依次对应 active/warning/
	 * passive/bus-off；STOPPED 与 SLEEPING 是设备级状态）。 */
	seq_printf(s, "state         %s\n",
		   snapshot.state == CAN_STATE_ERROR_ACTIVE  ? "error-active"  :
		   snapshot.state == CAN_STATE_ERROR_WARNING ? "error-warning" :
		   snapshot.state == CAN_STATE_ERROR_PASSIVE ? "error-passive" :
		   snapshot.state == CAN_STATE_BUS_OFF       ? "bus-off"       :
		   snapshot.state == CAN_STATE_STOPPED       ? "stopped"       :
							"sleeping");
	seq_printf(s, "queue_stopped %s\n",
		   snapshot.queue_stopped ? "yes" : "no");
	seq_printf(s, "armed_fault   %s\n", wbcan_fault_names[snapshot.fault]);
	seq_printf(s, "shots_left    %u\n", snapshot.fault_count);
	seq_printf(s, "skip_first    %u\n", snapshot.fault_after);
	if (snapshot.match_any)
		seq_puts(s, "match_id      any\n");
	else
		seq_printf(s, "match_id      %c:%x\n",
			   snapshot.match_id & CAN_EFF_FLAG ? 'e' : 's',
			   snapshot.match_id & CAN_EFF_MASK);
	seq_printf(s, "tx_frames     %llu\n", snapshot.stat_tx);
	seq_printf(s, "rx_frames     %llu\n", snapshot.stat_rx);
	seq_printf(s, "candidates    %llu\n", snapshot.stat_seen);
	seq_printf(s, "injected      %llu\n", snapshot.stat_injected);
	seq_printf(s, "restart_attempts %llu\n", snapshot.stat_restart_attempts);
	seq_printf(s, "stop_attempts %llu\n", snapshot.stat_stop_attempts);
	seq_printf(s, "bus_errors    %u\n", snapshot.bus_errors);

	return 0;
}

/* status 的 seq_file 打开：单实例读模型，私有数据取自 inode。 */
static int wbcan_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, wbcan_status_show, inode->i_private);
}

/* inject 的打开：把设备私有数据交给文件句柄，供写处理取回。 */
static int wbcan_inject_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

/*
 * inject：仅写文件。noop_llseek 拒绝一切 seek，保持纯命令流语义。
 */
static const struct file_operations wbcan_inject_fops = {
	.owner	= THIS_MODULE,
	.open	= wbcan_inject_open,
	.write	= wbcan_inject_write,
	.llseek	= noop_llseek,
};

/*
 * status：只读 seq_file。single_open()/single_release() 保证同时至多
 * 一个读者持有私有数据引用，读操作经 seq_read()/seq_lseek() 分页。
 */
static const struct file_operations wbcan_status_fops = {
	.owner	= THIS_MODULE,
	.open	= wbcan_status_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release = single_release,
};

/* --------------------------------------------------------------- 生命周期 */

static struct net_device *wbcan_dev;
static struct dentry *wbcan_dbg_root;
static bool fail_debugfs;
module_param(fail_debugfs, bool, 0400);
MODULE_PARM_DESC(fail_debugfs, "fail debugfs setup to test init cleanup");

/* debugfs 创建结果的统一判定：ERR_PTR 转负错误码，NULL 转 -ENODEV
 * （debugfs 被裁剪时），有效 dentry 返回 0。 */
static int wbcan_debugfs_err(const struct dentry *entry)
{
	if (IS_ERR(entry))
		return PTR_ERR(entry);
	return entry ? 0 : -ENODEV;
}

/*
 * 生命周期有意设计为仅支持单例：模块加载创建 wbcan0，
 * 模块卸载移除它。它不是一种 RTNL 链路类型，因此
 * `ip link add ... type wbcan` 有意不受支持。
 * wbcan_dev 与 wbcan_dbg_root 是模块级全局，由模块加载/卸载的生命周期
 * 串行化保护（同一时刻至多一个实例）。
 */
/*
 * 模块入口（__init）：创建并注册单例 wbcan0，再挂 debugfs。关键配置：
 * echo_skb_max = 0（回环在 start_xmit() 内自行完成，不走 echo 邮箱）、
 * IFF_ECHO（向 CAN 核心声明回环能力）、固定 1 Mbps 位速率（无物理线，
 * 仅为通过 open_candev() 的位时序校验）、ctrlmode_supported 含
 * LOOPBACK 与 BERR_REPORTING（套接字错误帧过滤依赖后者）、do_set_mode
 * 指向 wbcan_set_mode（bus-off 恢复入口）。失败回滚沿
 * err_debugfs → err_unregister → err_free 与创建顺序严格相反；
 * fail_debugfs 模块参数用于测试这条回滚路径。
 */
static int __init wbcan_init(void)
{
	struct wbcan_priv *priv;
	struct dentry *entry;
	int err;

	/*
	 * echo_skb_max 取 0：我们在 start_xmit 中自行回环，
	 * 而不是使用 can_put_echo_skb，因为故障平面需要决定
	 * 该帧到底要不要回来。
	 */
	wbcan_dev = alloc_candev(sizeof(struct wbcan_priv), 0);
	if (!wbcan_dev)
		return -ENOMEM;

	priv = netdev_priv(wbcan_dev);
	priv->dev = wbcan_dev;
	spin_lock_init(&priv->lock);
	u64_stats_init(&priv->stats_sync);
	priv->fault = WBCAN_FAULT_NONE;
	priv->match_any = true;

	wbcan_dev->netdev_ops = &wbcan_netdev_ops;
	wbcan_dev->ethtool_ops = &wbcan_ethtool_ops;
	/* IFF_ECHO：向 CAN 核心声明回环能力——can_send() 据此把需要本地
	 * 回环的帧标记为 PACKET_LOOPBACK，start_xmit() 据此决定是否克隆。 */
	wbcan_dev->flags |= IFF_ECHO;
	/* IFNAMSIZ（16）是接口名缓冲上限；"wbcan0" 占 6 字节。 */
	strscpy(wbcan_dev->name, "wbcan0", IFNAMSIZ);

	/*
	 * 没有真实的位时序：这里没有物理导线。声明固定比特率可避免
	 * `ip link set up` 索要时序参数，也明确表明
	 * 本设备并不建模物理层。那是板子上的 FW18 的职责。
	 */
	priv->can.bittiming.bitrate = 1000000;
	/* 1 Mbps 纯占位：满足 open_candev() 的「位时序已定义」校验。
	 * 真实位速率由板级 BSP（docs/architecture/robot-bsp-can-v0.1.md）
	 * 电气审批后的物理控制器决定，本设备不建模物理层。 */
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
				       CAN_CTRLMODE_BERR_REPORTING;
	/* bus-off 恢复回调：restart-ms 定时器与
	 * `ip link ... type can restart` 都经由它（见 wbcan_set_mode()）。 */
	priv->can.do_set_mode = wbcan_set_mode;
	WRITE_ONCE(priv->can.state, CAN_STATE_STOPPED);
	/*
	 * alloc_candev() 会让 TX 队列保持可运行直至 ndo_open()。
	 * 在单例已注册但未启用期间保持队列停止，
	 * 使一次全新加载拥有一份一致的停止状态快照。
	 */
	netif_stop_queue(wbcan_dev);

	/* register_candev() 内部完成 can_setup() 与 register_netdev()：
	 * 此后 wbcan0 对用户态可见。 */
	err = register_candev(wbcan_dev);
	if (err)
		goto err_free;

	/* debugfs：<KBUILD_MODNAME>/<dev>/ 下挂 inject（写）与 status（读）。 */
	wbcan_dbg_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
	err = wbcan_debugfs_err(wbcan_dbg_root);
	if (err) {
		wbcan_dbg_root = NULL;
		goto err_unregister;
	}
	priv->dbg_dir = debugfs_create_dir(wbcan_dev->name, wbcan_dbg_root);
	err = wbcan_debugfs_err(priv->dbg_dir);
	if (err) {
		priv->dbg_dir = NULL;
		goto err_debugfs;
	}
	/* fail_debugfs（仅加载时 0400 可设）：制造目录就绪后的失败，驱动
	 * err_debugfs 回滚路径——init 清理必须以测试覆盖。 */
	if (fail_debugfs) {
		err = -EIO;
		goto err_debugfs;
	}
	entry = debugfs_create_file("inject", 0200, priv->dbg_dir, priv,
				    &wbcan_inject_fops);
	err = wbcan_debugfs_err(entry);
	if (err)
		goto err_debugfs;
	entry = debugfs_create_file("status", 0444, priv->dbg_dir, priv,
				    &wbcan_status_fops);
	err = wbcan_debugfs_err(entry);
	if (err)
		goto err_debugfs;

	netdev_info(wbcan_dev, "registered; fault plane ready at %s/%s/inject\n",
		    KBUILD_MODNAME, wbcan_dev->name);
	return 0;

err_debugfs:
	debugfs_remove_recursive(wbcan_dbg_root);
	wbcan_dbg_root = NULL;
err_unregister:
	unregister_candev(wbcan_dev);
err_free:
	free_candev(wbcan_dev);
	wbcan_dev = NULL;
	return err;
}

/*
 * 模块退出（__exit）：与 init 成功路径严格逆序拆除——先摘 debugfs，再
 * 注销设备（注销会隐式触发 ndo_stop()），最后释放设备内存。
 */
static void __exit wbcan_exit(void)
{
	if (!wbcan_dev)
		return;

	debugfs_remove_recursive(wbcan_dbg_root);
	wbcan_dbg_root = NULL;
	unregister_candev(wbcan_dev);
	free_candev(wbcan_dev);
	wbcan_dev = NULL;
}

module_init(wbcan_init);
module_exit(wbcan_exit);

/* 模块元数据：GPL 许可与内核 CAN 核心导出符号的许可兼容；
 * 描述出现在 modinfo 与 sysfs 的模块属性中。 */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual CAN device with programmable fault injection");
