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
 * 为什么是内核模块而不是用户态程序：bus-off 状态、错误计数器与
 * 错误帧的生成都位于内核 CAN 核心中。用户态桥接器可以丢弃或篡改帧，
 * 却无法让 can_get_state() 报告 CAN_STATE_BUS_OFF，
 * 而这一迁移正是固件恢复路径所依赖的关键。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

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

#define WBCAN_ECHO_SKB_MAX	4

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

static bool fail_error_skb;
module_param(fail_error_skb, bool, 0600);
MODULE_PARM_DESC(fail_error_skb, "fail CAN error SKB allocation for testing");

static unsigned int test_restart_delay_ms;
module_param(test_restart_delay_ms, uint, 0600);
MODULE_PARM_DESC(test_restart_delay_ms,
		 "test-only delay before serializing CAN restart");

static unsigned int test_stop_delay_ms;
module_param(test_stop_delay_ms, uint, 0600);
MODULE_PARM_DESC(test_stop_delay_ms,
		 "test-only delay around TX drain and stop publication");

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
	 */
	u64			stat_tx;
	u64			stat_rx;
	u64			stat_injected;
	u64			stat_seen;
	u64			stat_restart_attempts;
	u64			stat_stop_attempts;
};

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
 *
 * debugfs 在私有锁内为故障平面拍快照，并在不持有 netdev TX 锁的
 * 情况下读取独立发布的 CAN 状态/队列位。格式化在私有锁之外进行，
 * 因此状态观测不会拉长 TX 关键路径。状态与队列的取值可能描述
 * 相邻的两个瞬间；它们是诊断遥测，不是控制权。
 */

/* ------------------------------------------------------------------ 辅助函数 */

/* 判定这一帧是否命中故障，命中则消耗一次机会。
 * 在持有锁的情况下调用。
 */
static canid_t wbcan_match_key(canid_t id)
{
	if (id & CAN_EFF_FLAG)
		return CAN_EFF_FLAG | (id & CAN_EFF_MASK);
	return id & CAN_SFF_MASK;
}

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

	default:
		if (!cf)
			return;
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] = CAN_ERR_CRTL_UNSPEC;
		break;
	}

	if (skb && netif_rx(skb) != NET_RX_SUCCESS)
		wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
}

/* ------------------------------------------------------------ netdev 操作 */

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

	if (can_dev_dropped_skb(dev, skb))
		return NETDEV_TX_OK;

	/* 回环我们自己的错误帧会形成循环。 */
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

	spin_lock_irqsave(&priv->lock, flags);
	wbcan_should_inject(priv, skb, &fault, &flip_byte, &flip_bit);
	spin_unlock_irqrestore(&priv->lock, flags);
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
	skb_tx_timestamp(skb);

	switch (fault) {
	case WBCAN_FAULT_BUS_OFF:
	case WBCAN_FAULT_ARB_LOST:
	case WBCAN_FAULT_STUFF_ERR:
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
	spin_lock_irqsave(&priv->lock, flags);
	priv->stat_tx++;
	spin_unlock_irqrestore(&priv->lock, flags);
	if (fault == WBCAN_FAULT_NONE &&
	    READ_ONCE(priv->can.state) == CAN_STATE_ERROR_WARNING)
		can_change_state(dev, NULL, CAN_STATE_ERROR_ACTIVE,
				 CAN_STATE_ERROR_ACTIVE);

	wbcan_stats_add(priv, 1, len, 0, 0, 0, 0, 0);
	loop = skb->pkt_type == PACKET_LOOPBACK;
	if (!loop) {
		consume_skb(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * 保留原始套接字，使 CAN_RAW_RECV_OWN_MSGS 与接收确认标志
	 * 维持标准的 SocketCAN 语义。位翻转需要私有数据副本，
	 * 因为包抓取点可能持有共享克隆。
	 */
	if (fault == WBCAN_FAULT_BIT_FLIP) {
		rx_skb = skb_copy(skb, GFP_ATOMIC);
		if (rx_skb)
			can_skb_set_owner(rx_skb, skb->sk);
		consume_skb(skb);
	} else {
		rx_skb = can_create_echo_skb(skb);
	}
	if (!rx_skb) {
		wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
		return NETDEV_TX_OK;
	}

	if (fault == WBCAN_FAULT_BIT_FLIP) {
		struct can_frame *rcf = (struct can_frame *)rx_skb->data;

		if (flip_byte < rcf->len) {
			rcf->data[flip_byte] ^= (1u << flip_bit);
			netdev_dbg(dev, "flipped byte %u bit %u of id 0x%x\n",
				   flip_byte, flip_bit, rcf->can_id);
		}
	}

	if (fault == WBCAN_FAULT_DROP_RX) {
		kfree_skb(rx_skb);
		wbcan_stats_add(priv, 0, 0, 0, 0, 0, 0, 1);
	} else {
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

static int wbcan_change_mtu(struct net_device *dev, int new_mtu)
{
	if (dev->flags & IFF_UP)
		return -EBUSY;
	if (new_mtu != CAN_MTU)
		return -EINVAL;

	WRITE_ONCE(dev->mtu, new_mtu);
	return 0;
}

static const struct net_device_ops wbcan_netdev_ops = {
	.ndo_open	= wbcan_open,
	.ndo_stop	= wbcan_stop,
	.ndo_start_xmit	= wbcan_start_xmit,
	.ndo_change_mtu	= wbcan_change_mtu,
	.ndo_get_stats64	= wbcan_get_stats64,
};

static int wbcan_get_sset_count(struct net_device *dev, int stringset)
{
	if (stringset == ETH_SS_STATS)
		return WBCAN_ETHTOOL_STAT_COUNT;
	return -EOPNOTSUPP;
}

static void wbcan_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	unsigned int index;

	if (stringset != ETH_SS_STATS)
		return;
	for (index = 0; index < WBCAN_ETHTOOL_STAT_COUNT; index++)
		strscpy(data + index * ETH_GSTRING_LEN,
			wbcan_ethtool_stat_names[index], ETH_GSTRING_LEN);
}

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

	argv = argv_split(GFP_KERNEL, buf, &argc);
	if (!argv)
		return -ENOMEM;
	if (argc < 1) {
		err = -EINVAL;
		goto out;
	}

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

	if (fault == WBCAN_FAULT_NONE) {
		if (argc > 2 || (argc == 2 && (kstrtou32(argv[1], 10, &count) || count))) {
			err = -EINVAL;
			goto out;
		}
		goto apply;
	}
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

static int wbcan_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, wbcan_status_show, inode->i_private);
}

static int wbcan_inject_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static const struct file_operations wbcan_inject_fops = {
	.owner	= THIS_MODULE,
	.open	= wbcan_inject_open,
	.write	= wbcan_inject_write,
	.llseek	= noop_llseek,
};

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
	wbcan_dev->flags |= IFF_ECHO;
	strscpy(wbcan_dev->name, "wbcan0", IFNAMSIZ);

	/*
	 * 没有真实的位时序：这里没有物理导线。声明固定比特率可避免
	 * `ip link set up` 索要时序参数，也明确表明
	 * 本设备并不建模物理层。那是板子上的 FW18 的职责。
	 */
	priv->can.bittiming.bitrate = 1000000;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
				       CAN_CTRLMODE_BERR_REPORTING;
	priv->can.do_set_mode = wbcan_set_mode;
	WRITE_ONCE(priv->can.state, CAN_STATE_STOPPED);
	/*
	 * alloc_candev() 会让 TX 队列保持可运行直至 ndo_open()。
	 * 在单例已注册但未启用期间保持队列停止，
	 * 使一次全新加载拥有一份一致的停止状态快照。
	 */
	netif_stop_queue(wbcan_dev);

	err = register_candev(wbcan_dev);
	if (err)
		goto err_free;

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

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual CAN device with programmable fault injection");
