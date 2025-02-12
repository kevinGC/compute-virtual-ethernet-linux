// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2021 Google, Inc.
 */

#include <linux/ethtool.h>
#include <linux/rtnetlink.h>
#include "gve.h"
#include "gve_adminq.h"
#include "gve_dqo.h"

static void gve_get_drvinfo(struct net_device *netdev,
			    struct ethtool_drvinfo *info)
{
	struct gve_priv *priv = netdev_priv(netdev);

	strscpy(info->driver, "gve", sizeof(info->driver));
	strscpy(info->version, gve_version_str, sizeof(info->version));
	strscpy(info->bus_info, pci_name(priv->pdev), sizeof(info->bus_info));
}

static void gve_set_msglevel(struct net_device *netdev, u32 value)
{
	struct gve_priv *priv = netdev_priv(netdev);

	priv->msg_enable = value;
}

static u32 gve_get_msglevel(struct net_device *netdev)
{
	struct gve_priv *priv = netdev_priv(netdev);

	return priv->msg_enable;
}

/* For the following stats column string names, make sure the order
 * matches how it is filled in the code. For xdp_aborted, xdp_drop,
 * xdp_pass, xdp_tx, xdp_redirect, make sure it also matches the order
 * as declared in enum xdp_action inside file uapi/linux/bpf.h .
 */
static const char gve_gstrings_main_stats[][ETH_GSTRING_LEN] = {
	"rx_packets", "rx_packets_sph", "rx_packets_hbo", "tx_packets",
	"rx_bytes", "tx_bytes", "rx_dropped", "tx_dropped", "tx_timeouts",
	"rx_skb_alloc_fail", "rx_buf_alloc_fail", "rx_desc_err_dropped_pkt",
	"rx_hsplit_err_dropped_pkt",
	"interface_up_cnt", "interface_down_cnt", "reset_cnt",
	"page_alloc_fail", "dma_mapping_error", "stats_report_trigger_cnt",
};

static const char gve_gstrings_rx_stats[][ETH_GSTRING_LEN] = {
	"rx_posted_desc[%u]", "rx_completed_desc[%u]", "rx_consumed_desc[%u]",
	"rx_bytes[%u]", "rx_header_bytes[%u]",
	"rx_cont_packet_cnt[%u]", "rx_frag_flip_cnt[%u]", "rx_frag_copy_cnt[%u]",
	"rx_frag_alloc_cnt[%u]",
	"rx_dropped_pkt[%u]", "rx_copybreak_pkt[%u]", "rx_copied_pkt[%u]",
	"rx_queue_drop_cnt[%u]", "rx_no_buffers_posted[%u]",
	"rx_drops_packet_over_mru[%u]", "rx_drops_invalid_checksum[%u]",
	"rx_xdp_aborted[%u]", "rx_xdp_drop[%u]", "rx_xdp_pass[%u]",
	"rx_xdp_tx[%u]", "rx_xdp_redirect[%u]",
	"rx_xdp_tx_errors[%u]", "rx_xdp_redirect_errors[%u]", "rx_xdp_alloc_fails[%u]",
};

static const char gve_gstrings_tx_stats[][ETH_GSTRING_LEN] = {
	"tx_posted_desc[%u]", "tx_completed_desc[%u]", "tx_consumed_desc[%u]", "tx_bytes[%u]",
	"tx_wake[%u]", "tx_stop[%u]", "tx_event_counter[%u]",
	"tx_dma_mapping_error[%u]", "tx_xsk_wakeup[%u]",
	"tx_xsk_done[%u]", "tx_xsk_sent[%u]", "tx_xdp_xmit[%u]", "tx_xdp_xmit_errors[%u]"
};

static const char gve_gstrings_adminq_stats[][ETH_GSTRING_LEN] = {
	"adminq_prod_cnt", "adminq_cmd_fail", "adminq_timeouts",
	"adminq_describe_device_cnt", "adminq_cfg_device_resources_cnt",
	"adminq_register_page_list_cnt", "adminq_unregister_page_list_cnt",
	"adminq_create_tx_queue_cnt", "adminq_create_rx_queue_cnt",
	"adminq_destroy_tx_queue_cnt", "adminq_destroy_rx_queue_cnt",
	"adminq_dcfg_device_resources_cnt", "adminq_set_driver_parameter_cnt",
	"adminq_report_stats_cnt", "adminq_report_link_speed_cnt",
	"adminq_cfg_flow_rule", "adminq_cfg_rss_cnt"
};

static const char gve_gstrings_priv_flags[][ETH_GSTRING_LEN] = {
	"report-stats", "enable-header-split", "enable-strict-header-split",
	"enable-max-rx-buffer-size"
};

#define GVE_MAIN_STATS_LEN  ARRAY_SIZE(gve_gstrings_main_stats)
#define GVE_ADMINQ_STATS_LEN  ARRAY_SIZE(gve_gstrings_adminq_stats)
#define NUM_GVE_TX_CNTS	ARRAY_SIZE(gve_gstrings_tx_stats)
#define NUM_GVE_RX_CNTS	ARRAY_SIZE(gve_gstrings_rx_stats)
#define GVE_PRIV_FLAGS_STR_LEN ARRAY_SIZE(gve_gstrings_priv_flags)

static void gve_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct gve_priv *priv = netdev_priv(netdev);
	char *s = (char *)data;
	int num_tx_queues;
	int i, j;

	num_tx_queues = gve_num_tx_queues(priv);
	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(s, *gve_gstrings_main_stats,
		       sizeof(gve_gstrings_main_stats));
		s += sizeof(gve_gstrings_main_stats);

		for (i = 0; i < priv->rx_cfg.num_queues; i++) {
			for (j = 0; j < NUM_GVE_RX_CNTS; j++) {
				snprintf(s, ETH_GSTRING_LEN,
					 gve_gstrings_rx_stats[j], i);
				s += ETH_GSTRING_LEN;
			}
		}

		for (i = 0; i < num_tx_queues; i++) {
			for (j = 0; j < NUM_GVE_TX_CNTS; j++) {
				snprintf(s, ETH_GSTRING_LEN,
					 gve_gstrings_tx_stats[j], i);
				s += ETH_GSTRING_LEN;
			}
		}

		memcpy(s, *gve_gstrings_adminq_stats,
		       sizeof(gve_gstrings_adminq_stats));
		s += sizeof(gve_gstrings_adminq_stats);
		break;

	case ETH_SS_PRIV_FLAGS:
		memcpy(s, *gve_gstrings_priv_flags,
		       sizeof(gve_gstrings_priv_flags));
		s += sizeof(gve_gstrings_priv_flags);
		break;

	default:
		break;
	}
}

static int gve_get_sset_count(struct net_device *netdev, int sset)
{
	struct gve_priv *priv = netdev_priv(netdev);
	int num_tx_queues;

	num_tx_queues = gve_num_tx_queues(priv);
	switch (sset) {
	case ETH_SS_STATS:
		return GVE_MAIN_STATS_LEN + GVE_ADMINQ_STATS_LEN +
		       (priv->rx_cfg.num_queues * NUM_GVE_RX_CNTS) +
		       (num_tx_queues * NUM_GVE_TX_CNTS);
	case ETH_SS_PRIV_FLAGS:
		return GVE_PRIV_FLAGS_STR_LEN;
	default:
		return -EOPNOTSUPP;
	}
}

static void
gve_get_ethtool_stats(struct net_device *netdev,
		      struct ethtool_stats *stats, u64 *data)
{
	u64 tmp_rx_pkts, tmp_rx_pkts_sph, tmp_rx_pkts_hbo, tmp_rx_bytes,
		tmp_rx_hbytes, tmp_rx_skb_alloc_fail, tmp_rx_buf_alloc_fail,
		tmp_rx_desc_err_dropped_pkt, tmp_rx_hsplit_err_dropped_pkt,
		tmp_tx_pkts, tmp_tx_bytes;
	u64 rx_buf_alloc_fail, rx_desc_err_dropped_pkt, rx_hsplit_err_dropped_pkt,
		rx_pkts, rx_pkts_sph, rx_pkts_hbo, rx_skb_alloc_fail, rx_bytes,
		tx_pkts, tx_bytes, tx_dropped;
	int stats_idx, base_stats_idx, max_stats_idx;
	struct stats *report_stats;
	int *rx_qid_to_stats_idx;
	int *tx_qid_to_stats_idx;
	struct gve_priv *priv;
	bool skip_nic_stats;
	unsigned int start;
	int num_tx_queues;
	int ring;
	int i, j;

	ASSERT_RTNL();

	priv = netdev_priv(netdev);
	num_tx_queues = gve_num_tx_queues(priv);
	report_stats = priv->stats_report->stats;
	rx_qid_to_stats_idx = kmalloc_array(priv->rx_cfg.num_queues,
					    sizeof(int), GFP_KERNEL);
	if (!rx_qid_to_stats_idx)
		return;
	tx_qid_to_stats_idx = kmalloc_array(num_tx_queues,
					    sizeof(int), GFP_KERNEL);
	if (!tx_qid_to_stats_idx) {
		kfree(rx_qid_to_stats_idx);
		return;
	}
	for (rx_pkts = 0, rx_bytes = 0, rx_pkts_sph = 0, rx_pkts_hbo = 0,
	     rx_skb_alloc_fail = 0, rx_buf_alloc_fail = 0,
	     rx_desc_err_dropped_pkt = 0, rx_hsplit_err_dropped_pkt = 0,
	     ring = 0;
	     ring < priv->rx_cfg.num_queues; ring++) {
		if (priv->rx) {
			do {
				struct gve_rx_ring *rx = &priv->rx[ring];

				start =
				  u64_stats_fetch_begin(&priv->rx[ring].statss);
				tmp_rx_pkts = rx->rpackets;
				tmp_rx_pkts_sph = rx->rx_hsplit_pkt;
				tmp_rx_pkts_hbo = rx->rx_hsplit_hbo_pkt;
				tmp_rx_bytes = rx->rbytes;
				tmp_rx_skb_alloc_fail = rx->rx_skb_alloc_fail;
				tmp_rx_buf_alloc_fail = rx->rx_buf_alloc_fail;
				tmp_rx_desc_err_dropped_pkt =
					rx->rx_desc_err_dropped_pkt;
				tmp_rx_hsplit_err_dropped_pkt =
					rx->rx_hsplit_err_dropped_pkt;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
			rx_pkts += tmp_rx_pkts;
			rx_pkts_sph += tmp_rx_pkts_sph;
			rx_pkts_hbo += tmp_rx_pkts_hbo;
			rx_bytes += tmp_rx_bytes;
			rx_skb_alloc_fail += tmp_rx_skb_alloc_fail;
			rx_buf_alloc_fail += tmp_rx_buf_alloc_fail;
			rx_desc_err_dropped_pkt += tmp_rx_desc_err_dropped_pkt;
			rx_hsplit_err_dropped_pkt += tmp_rx_hsplit_err_dropped_pkt;
		}
	}
	for (tx_pkts = 0, tx_bytes = 0, tx_dropped = 0, ring = 0;
	     ring < num_tx_queues; ring++) {
		if (priv->tx) {
			do {
				start =
				  u64_stats_fetch_begin(&priv->tx[ring].statss);
				tmp_tx_pkts = priv->tx[ring].pkt_done;
				tmp_tx_bytes = priv->tx[ring].bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
			tx_pkts += tmp_tx_pkts;
			tx_bytes += tmp_tx_bytes;
			tx_dropped += priv->tx[ring].dropped_pkt;
		}
	}

	i = 0;
	data[i++] = rx_pkts;
	data[i++] = rx_pkts_sph;
	data[i++] = rx_pkts_hbo;
	data[i++] = tx_pkts;
	data[i++] = rx_bytes;
	data[i++] = tx_bytes;
	/* total rx dropped packets */
	data[i++] = rx_skb_alloc_fail + rx_buf_alloc_fail +
		    rx_desc_err_dropped_pkt;
	data[i++] = tx_dropped;
	data[i++] = priv->tx_timeo_cnt;
	data[i++] = rx_skb_alloc_fail;
	data[i++] = rx_buf_alloc_fail;
	data[i++] = rx_desc_err_dropped_pkt;
	data[i++] = rx_hsplit_err_dropped_pkt;
	data[i++] = priv->interface_up_cnt;
	data[i++] = priv->interface_down_cnt;
	data[i++] = priv->reset_cnt;
	data[i++] = priv->page_alloc_fail;
	data[i++] = priv->dma_mapping_error;
	data[i++] = priv->stats_report_trigger_cnt;
	i = GVE_MAIN_STATS_LEN;

	/* For rx cross-reporting stats, start from nic rx stats in report */
	base_stats_idx = GVE_TX_STATS_REPORT_NUM * num_tx_queues +
		GVE_RX_STATS_REPORT_NUM * priv->rx_cfg.num_queues;
	max_stats_idx = NIC_RX_STATS_REPORT_NUM * priv->rx_cfg.num_queues +
		base_stats_idx;
	/* Preprocess the stats report for rx, map queue id to start index */
	skip_nic_stats = false;
	for (stats_idx = base_stats_idx; stats_idx < max_stats_idx;
		stats_idx += NIC_RX_STATS_REPORT_NUM) {
		u32 stat_name = be32_to_cpu(report_stats[stats_idx].stat_name);
		u32 queue_id = be32_to_cpu(report_stats[stats_idx].queue_id);

		if (stat_name == 0) {
			/* no stats written by NIC yet */
			skip_nic_stats = true;
			break;
		}
		rx_qid_to_stats_idx[queue_id] = stats_idx;
	}
	/* walk RX rings */
	if (priv->rx) {
		for (ring = 0; ring < priv->rx_cfg.num_queues; ring++) {
			struct gve_rx_ring *rx = &priv->rx[ring];

			data[i++] = rx->fill_cnt;
			data[i++] = rx->cnt;
			data[i++] = rx->fill_cnt - rx->cnt;
			do {
				start =
				  u64_stats_fetch_begin(&priv->rx[ring].statss);
				tmp_rx_bytes = rx->rbytes;
				tmp_rx_hbytes = rx->rheader_bytes;
				tmp_rx_skb_alloc_fail = rx->rx_skb_alloc_fail;
				tmp_rx_buf_alloc_fail = rx->rx_buf_alloc_fail;
				tmp_rx_desc_err_dropped_pkt =
					rx->rx_desc_err_dropped_pkt;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
			data[i++] = tmp_rx_bytes;
			data[i++] = tmp_rx_hbytes;
			data[i++] = rx->rx_cont_packet_cnt;
			data[i++] = rx->rx_frag_flip_cnt;
			data[i++] = rx->rx_frag_copy_cnt;
			data[i++] = rx->rx_frag_alloc_cnt;
			/* rx dropped packets */
			data[i++] = tmp_rx_skb_alloc_fail +
				tmp_rx_buf_alloc_fail +
				tmp_rx_desc_err_dropped_pkt;
			data[i++] = rx->rx_copybreak_pkt;
			data[i++] = rx->rx_copied_pkt;
			/* stats from NIC */
			if (skip_nic_stats) {
				/* skip NIC rx stats */
				i += NIC_RX_STATS_REPORT_NUM;
			} else {
				stats_idx = rx_qid_to_stats_idx[ring];
				for (j = 0; j < NIC_RX_STATS_REPORT_NUM; j++) {
					u64 value =
						be64_to_cpu(report_stats[stats_idx + j].value);

					data[i++] = value;
				}
			}
			/* XDP rx counters */
			do {
				start =	u64_stats_fetch_begin(&priv->rx[ring].statss);
				for (j = 0; j < GVE_XDP_ACTIONS; j++)
					data[i + j] = rx->xdp_actions[j];
				data[i + j++] = rx->xdp_tx_errors;
				data[i + j++] = rx->xdp_redirect_errors;
				data[i + j++] = rx->xdp_alloc_fails;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
			i += GVE_XDP_ACTIONS + 3; /* XDP rx counters */
		}
	} else {
		i += priv->rx_cfg.num_queues * NUM_GVE_RX_CNTS;
	}

	/* For tx cross-reporting stats, start from nic tx stats in report */
	base_stats_idx = max_stats_idx;
	max_stats_idx = NIC_TX_STATS_REPORT_NUM * num_tx_queues +
		max_stats_idx;
	/* Preprocess the stats report for tx, map queue id to start index */
	skip_nic_stats = false;
	for (stats_idx = base_stats_idx; stats_idx < max_stats_idx;
		stats_idx += NIC_TX_STATS_REPORT_NUM) {
		u32 stat_name = be32_to_cpu(report_stats[stats_idx].stat_name);
		u32 queue_id = be32_to_cpu(report_stats[stats_idx].queue_id);

		if (stat_name == 0) {
			/* no stats written by NIC yet */
			skip_nic_stats = true;
			break;
		}
		tx_qid_to_stats_idx[queue_id] = stats_idx;
	}
	/* walk TX rings */
	if (priv->tx) {
		for (ring = 0; ring < num_tx_queues; ring++) {
			struct gve_tx_ring *tx = &priv->tx[ring];

			if (gve_is_gqi(priv)) {
				data[i++] = tx->req;
				data[i++] = tx->done;
				data[i++] = tx->req - tx->done;
			} else {
				/* DQO doesn't currently support
				 * posted/completed descriptor counts;
				 */
				data[i++] = 0;
				data[i++] = 0;
				data[i++] = tx->dqo_tx.tail - tx->dqo_tx.head;
			}
			do {
				start =
				  u64_stats_fetch_begin(&priv->tx[ring].statss);
				tmp_tx_bytes = tx->bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
			data[i++] = tmp_tx_bytes;
			data[i++] = tx->wake_queue;
			data[i++] = tx->stop_queue;
			data[i++] = gve_tx_load_event_counter(priv, tx);
			data[i++] = tx->dma_mapping_error;
			/* stats from NIC */
			if (skip_nic_stats) {
				/* skip NIC tx stats */
				i += NIC_TX_STATS_REPORT_NUM;
			} else {
				stats_idx = tx_qid_to_stats_idx[ring];
				for (j = 0; j < NIC_TX_STATS_REPORT_NUM; j++) {
					u64 value =
						be64_to_cpu(report_stats[stats_idx + j].value);
					data[i++] = value;
				}
			}
			/* XDP xsk counters */
			data[i++] = tx->xdp_xsk_wakeup;
			data[i++] = tx->xdp_xsk_done;
			do {
				start = u64_stats_fetch_begin(&priv->tx[ring].statss);
				data[i] = tx->xdp_xsk_sent;
				data[i + 1] = tx->xdp_xmit;
				data[i + 2] = tx->xdp_xmit_errors;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
			i += 3; /* XDP tx counters */
		}
	} else {
		i += num_tx_queues * NUM_GVE_TX_CNTS;
	}

	kfree(rx_qid_to_stats_idx);
	kfree(tx_qid_to_stats_idx);
	/* AQ Stats */
	data[i++] = priv->adminq_prod_cnt;
	data[i++] = priv->adminq_cmd_fail;
	data[i++] = priv->adminq_timeouts;
	data[i++] = priv->adminq_describe_device_cnt;
	data[i++] = priv->adminq_cfg_device_resources_cnt;
	data[i++] = priv->adminq_register_page_list_cnt;
	data[i++] = priv->adminq_unregister_page_list_cnt;
	data[i++] = priv->adminq_create_tx_queue_cnt;
	data[i++] = priv->adminq_create_rx_queue_cnt;
	data[i++] = priv->adminq_destroy_tx_queue_cnt;
	data[i++] = priv->adminq_destroy_rx_queue_cnt;
	data[i++] = priv->adminq_dcfg_device_resources_cnt;
	data[i++] = priv->adminq_set_driver_parameter_cnt;
	data[i++] = priv->adminq_report_stats_cnt;
	data[i++] = priv->adminq_report_link_speed_cnt;
	data[i++] = priv->adminq_cfg_flow_rule_cnt;
	data[i++] = priv->adminq_cfg_rss_cnt;
}

static void gve_get_channels(struct net_device *netdev,
			     struct ethtool_channels *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);

	cmd->max_rx = priv->rx_cfg.max_queues;
	cmd->max_tx = priv->tx_cfg.max_queues;
	cmd->max_other = 0;
	cmd->max_combined = 0;
	cmd->rx_count = priv->rx_cfg.num_queues;
	cmd->tx_count = priv->tx_cfg.num_queues;
	cmd->other_count = 0;
	cmd->combined_count = 0;
}

static int gve_set_channels(struct net_device *netdev,
			    struct ethtool_channels *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);
	struct gve_queue_config new_tx_cfg = priv->tx_cfg;
	struct gve_queue_config new_rx_cfg = priv->rx_cfg;
	struct ethtool_channels old_settings;
	int new_tx = cmd->tx_count;
	int new_rx = cmd->rx_count;

	gve_get_channels(netdev, &old_settings);

	/* Changing combined is not allowed */
	if (cmd->combined_count != old_settings.combined_count)
		return -EINVAL;

	if (!new_rx || !new_tx)
		return -EINVAL;

	if (priv->num_xdp_queues &&
	    (new_tx != new_rx || (2 * new_tx > priv->tx_cfg.max_queues))) {
		dev_err(&priv->pdev->dev, "XDP load failed: The number of configured RX queues should be equal to the number of configured TX queues and the number of configured RX/TX queues should be less than or equal to half the maximum number of RX/TX queues");
		return -EINVAL;
	}

	if (!netif_carrier_ok(netdev)) {
		priv->tx_cfg.num_queues = new_tx;
		priv->rx_cfg.num_queues = new_rx;
		return 0;
	}

	new_tx_cfg.num_queues = new_tx;
	new_rx_cfg.num_queues = new_rx;

	return gve_adjust_queues(priv, new_rx_cfg, new_tx_cfg);
}

static void gve_get_ringparam(struct net_device *netdev,
			      struct ethtool_ringparam *cmd,
			      struct kernel_ethtool_ringparam *kernel_cmd,
			      struct netlink_ext_ack *extack)
{
	struct gve_priv *priv = netdev_priv(netdev);
	cmd->rx_max_pending = priv->max_rx_desc_cnt;
	cmd->tx_max_pending = priv->max_tx_desc_cnt;
	cmd->rx_pending = priv->rx_desc_cnt;
	cmd->tx_pending = priv->tx_desc_cnt;
}

static int gve_set_ringparam(struct net_device *netdev,
			     struct ethtool_ringparam *cmd,
			     struct kernel_ethtool_ringparam *kernel_cmd,
			     struct netlink_ext_ack *extack)
{
	struct gve_priv *priv = netdev_priv(netdev);
	int old_rx_desc_cnt = priv->rx_desc_cnt;
	int old_tx_desc_cnt = priv->tx_desc_cnt;
	int new_tx_desc_cnt = cmd->tx_pending;
	int new_rx_desc_cnt = cmd->rx_pending;
	int new_max_registered_pages =
		new_rx_desc_cnt * gve_num_rx_qpls(priv) +
			GVE_TX_PAGE_COUNT * gve_num_tx_qpls(priv);

	if (new_tx_desc_cnt < GVE_RING_LENGTH_LIMIT_MIN ||
		new_rx_desc_cnt < GVE_RING_LENGTH_LIMIT_MIN) {
		dev_err(&priv->pdev->dev, "Ring size cannot be less than %d\n",
			GVE_RING_LENGTH_LIMIT_MIN);
		return -EINVAL;
	}

	if (new_tx_desc_cnt > GVE_RING_LENGTH_LIMIT_MAX ||
		new_rx_desc_cnt > GVE_RING_LENGTH_LIMIT_MAX) {
		dev_err(&priv->pdev->dev,
			"Ring size cannot be greater than %d\n",
			GVE_RING_LENGTH_LIMIT_MAX);
		return -EINVAL;
	}

	/* Ring size must be a power of 2, will fail if passed values are not
	 * In the future we may want to update to round down to the
	 * closest valid ring size
	 */
	if ((new_tx_desc_cnt & (new_tx_desc_cnt - 1)) != 0 ||
		(new_rx_desc_cnt & (new_rx_desc_cnt - 1)) != 0) {
		dev_err(&priv->pdev->dev, "Ring size must be a power of 2\n");
		return -EINVAL;
	}

	if (new_tx_desc_cnt > priv->max_tx_desc_cnt) {
		dev_err(&priv->pdev->dev,
			"Tx ring size passed %d is larger than max tx ring size %u\n",
			new_tx_desc_cnt, priv->max_tx_desc_cnt);
		return -EINVAL;
	}

	if (new_rx_desc_cnt > priv->max_rx_desc_cnt) {
		dev_err(&priv->pdev->dev,
			"Rx ring size passed %d is larger than max rx ring size %u\n",
			new_rx_desc_cnt, priv->max_rx_desc_cnt);
		return -EINVAL;
	}

	if (new_max_registered_pages > priv->max_registered_pages) {
		dev_err(&priv->pdev->dev,
				"Allocating too many pages %d; max %llu",
				new_max_registered_pages,
				priv->max_registered_pages);
		return -EINVAL;
	}

	// Nothing to change return success
	if (new_tx_desc_cnt == old_tx_desc_cnt && new_rx_desc_cnt == old_rx_desc_cnt)
		return 0;

	return gve_adjust_ring_sizes(priv, new_tx_desc_cnt, new_rx_desc_cnt);
}

static int gve_user_reset(struct net_device *netdev, u32 *flags)
{
	struct gve_priv *priv = netdev_priv(netdev);

	if (*flags == ETH_RESET_ALL) {
		*flags = 0;
		return gve_reset(priv, true);
	}

	return -EOPNOTSUPP;
}

static int gve_get_tunable(struct net_device *netdev,
			   const struct ethtool_tunable *etuna, void *value)
{
	struct gve_priv *priv = netdev_priv(netdev);

	switch (etuna->id) {
	case ETHTOOL_RX_COPYBREAK:
		*(u32 *)value = priv->rx_copybreak;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int gve_set_tunable(struct net_device *netdev,
			   const struct ethtool_tunable *etuna,
			   const void *value)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u32 len;

	switch (etuna->id) {
	case ETHTOOL_RX_COPYBREAK:
	{
		u32 max_copybreak = gve_is_gqi(priv) ?
			(PAGE_SIZE / 2) : priv->data_buffer_size_dqo;

		len = *(u32 *)value;
		if (len > max_copybreak)
			return -EINVAL;
		priv->rx_copybreak = len;
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static u32 gve_get_priv_flags(struct net_device *netdev)
{
	struct gve_priv *priv = netdev_priv(netdev);
	return priv->ethtool_flags & GVE_PRIV_FLAGS_MASK;
}

static int gve_set_priv_flags(struct net_device *netdev, u32 flags)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u64 ori_flags, new_flags, flag_diff;
	int new_packet_buffer_size;
	int num_tx_queues;

	/* If turning off header split, strict header split will be turned off too*/
	if (gve_get_enable_header_split(priv) &&
		!(flags & BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT))) {
		flags &= ~BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT);
		flags &= ~BIT(GVE_PRIV_FLAGS_ENABLE_STRICT_HEADER_SPLIT);
	}

	/* If strict header-split is requested, turn on regular header-split */
	if (flags & BIT(GVE_PRIV_FLAGS_ENABLE_STRICT_HEADER_SPLIT))
		flags |= BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT);

	/* Make sure header-split is available */
	if ((flags & BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT)) &&
		!(priv->ethtool_defaults & BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT))) {
		dev_err(&priv->pdev->dev,
			"Header-split not available\n");
		return -EINVAL;
	}

	if ((flags & BIT(GVE_PRIV_FLAGS_ENABLE_MAX_RX_BUFFER_SIZE)) &&
			priv->dev_max_rx_buffer_size <= GVE_MIN_RX_BUFFER_SIZE) {
		dev_err(&priv->pdev->dev,
			"Max-rx-buffer-size not available\n");
		return -EINVAL;
	}

	num_tx_queues = gve_num_tx_queues(priv);
	ori_flags = READ_ONCE(priv->ethtool_flags);

	new_flags = flags & GVE_PRIV_FLAGS_MASK;

	flag_diff = new_flags ^ ori_flags;

	if ((flag_diff & BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT)) ||
		(flag_diff & BIT(GVE_PRIV_FLAGS_ENABLE_MAX_RX_BUFFER_SIZE))) {
		bool enable_hdr_split =
			new_flags & BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT);
		bool enable_max_buffer_size =
			new_flags & BIT(GVE_PRIV_FLAGS_ENABLE_MAX_RX_BUFFER_SIZE);
		int err;

		if (enable_max_buffer_size)
			new_packet_buffer_size = priv->dev_max_rx_buffer_size;
		else
			new_packet_buffer_size = GVE_RX_BUFFER_SIZE_DQO;

		err = gve_reconfigure_rx_rings(priv,
					      enable_hdr_split,
					      new_packet_buffer_size);
		if (err)
			return err;
	}

	priv->ethtool_flags = new_flags;

	/* start report-stats timer when user turns report stats on. */
	if (flags & BIT(0)) {
		mod_timer(&priv->stats_report_timer,
			  round_jiffies(jiffies +
					msecs_to_jiffies(priv->stats_report_timer_period)));
	}
	/* Zero off gve stats when report-stats turned off and */
	/* delete report stats timer. */
	if (!(flags & BIT(0)) && (ori_flags & BIT(0))) {
		int tx_stats_num = GVE_TX_STATS_REPORT_NUM *
			num_tx_queues;
		int rx_stats_num = GVE_RX_STATS_REPORT_NUM *
			priv->rx_cfg.num_queues;

		memset(priv->stats_report->stats, 0, (tx_stats_num + rx_stats_num) *
				   sizeof(struct stats));
		del_timer_sync(&priv->stats_report_timer);
	}
	priv->header_split_strict =
		(priv->ethtool_flags &
		 BIT(GVE_PRIV_FLAGS_ENABLE_STRICT_HEADER_SPLIT)) ? true : false;

	return 0;
}

static int gve_get_link_ksettings(struct net_device *netdev,
				  struct ethtool_link_ksettings *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);
	int err = 0;

	if (priv->link_speed == 0)
		err = gve_adminq_report_link_speed(priv);

	cmd->base.speed = priv->link_speed;
	return err;
}

static int gve_get_coalesce(struct net_device *netdev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_ec,
			    struct netlink_ext_ack *extack)
{
	struct gve_priv *priv = netdev_priv(netdev);

	if (gve_is_gqi(priv))
		return -EOPNOTSUPP;
	ec->tx_coalesce_usecs = priv->tx_coalesce_usecs;
	ec->rx_coalesce_usecs = priv->rx_coalesce_usecs;

	return 0;
}

static int gve_set_coalesce(struct net_device *netdev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_ec,
			    struct netlink_ext_ack *extack)
{
	struct gve_priv *priv = netdev_priv(netdev);
	u32 tx_usecs_orig = priv->tx_coalesce_usecs;
	u32 rx_usecs_orig = priv->rx_coalesce_usecs;
	int idx;

	if (gve_is_gqi(priv))
		return -EOPNOTSUPP;

	if (ec->tx_coalesce_usecs > GVE_MAX_ITR_INTERVAL_DQO ||
	    ec->rx_coalesce_usecs > GVE_MAX_ITR_INTERVAL_DQO)
		return -EINVAL;
	priv->tx_coalesce_usecs = ec->tx_coalesce_usecs;
	priv->rx_coalesce_usecs = ec->rx_coalesce_usecs;

	if (tx_usecs_orig != priv->tx_coalesce_usecs) {
		for (idx = 0; idx < priv->tx_cfg.num_queues; idx++) {
			int ntfy_idx = gve_tx_idx_to_ntfy(priv, idx);
			struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

			gve_set_itr_coalesce_usecs_dqo(priv, block,
						       priv->tx_coalesce_usecs);
		}
	}

	if (rx_usecs_orig != priv->rx_coalesce_usecs) {
		for (idx = 0; idx < priv->rx_cfg.num_queues; idx++) {
			int ntfy_idx = gve_rx_idx_to_ntfy(priv, idx);
			struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

			gve_set_itr_coalesce_usecs_dqo(priv, block,
						       priv->rx_coalesce_usecs);
		}
	}

	return 0;
}

static u32 gve_get_rxfh_key_size(struct net_device *netdev)
{
	return GVE_RSS_KEY_SIZE;
}

static u32 gve_get_rxfh_indir_size(struct net_device *netdev)
{
	return GVE_RSS_INDIR_SIZE;
}

static int gve_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key,
			u8 *hfunc)
{
	struct gve_priv *priv = netdev_priv(netdev);
	struct gve_rss_config *rss_config = &priv->rss_config;
	u16 i;

	if (hfunc) {
		switch (rss_config->alg) {
		case GVE_RSS_HASH_TOEPLITZ:
			*hfunc = ETH_RSS_HASH_TOP;
			break;
		case GVE_RSS_HASH_UNDEFINED:
		default:
			return -EOPNOTSUPP;
		}
	}
	if (key)
		memcpy(key, rss_config->key, rss_config->key_size);

	if (indir)
		/* Each 32 bits pointed by 'indir' is stored with a lut entry */
		for (i = 0; i < rss_config->indir_size; i++)
			indir[i] = (u32)rss_config->indir[i];

	return 0;
}

static int gve_set_rxfh(struct net_device *netdev, const u32 *indir,
			const u8 *key, const u8 hfunc)
{
	struct gve_priv *priv = netdev_priv(netdev);
	struct gve_rss_config *rss_config = &priv->rss_config;
	bool init = false;
	u16 i;
	int err = 0;

	/* Initialize RSS if not configured before */
	if (rss_config->alg == GVE_RSS_HASH_UNDEFINED) {
		err = gve_rss_config_init(priv);
		if (err)
			return err;
		init = true;
	}

	switch (hfunc) {
	case ETH_RSS_HASH_NO_CHANGE:
		break;
	case ETH_RSS_HASH_TOP:
		rss_config->alg = GVE_RSS_HASH_TOEPLITZ;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (!key && !indir && !init)
		return 0;

	if (key)
		memcpy(rss_config->key, key, rss_config->key_size);

	if (indir) {
		/* Each 32 bits pointed by 'indir' is stored with a lut entry */
		for (i = 0; i < rss_config->indir_size; i++)
			rss_config->indir[i] = indir[i];
	}

	return gve_adminq_configure_rss(priv, rss_config);
}

static const char *gve_flow_type_name(enum gve_adminq_flow_type flow_type)
{
	switch (flow_type) {
	case GVE_FLOW_TYPE_TCPV4:
	case GVE_FLOW_TYPE_TCPV6:
		return "TCP";
	case GVE_FLOW_TYPE_UDPV4:
	case GVE_FLOW_TYPE_UDPV6:
		return "UDP";
	case GVE_FLOW_TYPE_SCTPV4:
	case GVE_FLOW_TYPE_SCTPV6:
		return "SCTP";
	case GVE_FLOW_TYPE_AHV4:
	case GVE_FLOW_TYPE_AHV6:
		return "AH";
	case GVE_FLOW_TYPE_ESPV4:
	case GVE_FLOW_TYPE_ESPV6:
		return "ESP";
	}
	return NULL;
}

static void gve_print_flow_rule(struct gve_priv *priv,
				struct gve_flow_rule *rule)
{
	const char *proto = gve_flow_type_name(rule->flow_type);

	if (!proto)
		return;

	switch (rule->flow_type) {
	case GVE_FLOW_TYPE_TCPV4:
	case GVE_FLOW_TYPE_UDPV4:
	case GVE_FLOW_TYPE_SCTPV4:
		dev_info(&priv->pdev->dev, "Rule ID: %u dst_ip: %pI4 src_ip %pI4 %s: dst_port %hu src_port %hu\n",
			 rule->loc,
			 &rule->key.dst_ip[0],
			 &rule->key.src_ip[0],
			 proto,
			 ntohs(rule->key.dst_port),
			 ntohs(rule->key.src_port));
		break;
	case GVE_FLOW_TYPE_AHV4:
	case GVE_FLOW_TYPE_ESPV4:
		dev_info(&priv->pdev->dev, "Rule ID: %u dst_ip: %pI4 src_ip %pI4 %s: spi %hu\n",
			 rule->loc,
			 &rule->key.dst_ip[0],
			 &rule->key.src_ip[0],
			 proto,
			 ntohl(rule->key.spi));
		break;
	case GVE_FLOW_TYPE_TCPV6:
	case GVE_FLOW_TYPE_UDPV6:
	case GVE_FLOW_TYPE_SCTPV6:
		dev_info(&priv->pdev->dev, "Rule ID: %u dst_ip: %pI6 src_ip %pI6 %s: dst_port %hu src_port %hu\n",
			 rule->loc,
			 &rule->key.dst_ip,
			 &rule->key.src_ip,
			 proto,
			 ntohs(rule->key.dst_port),
			 ntohs(rule->key.src_port));
		break;
	case GVE_FLOW_TYPE_AHV6:
	case GVE_FLOW_TYPE_ESPV6:
		dev_info(&priv->pdev->dev, "Rule ID: %u dst_ip: %pI6 src_ip %pI6 %s: spi %hu\n",
			 rule->loc,
			 &rule->key.dst_ip,
			 &rule->key.src_ip,
			 proto,
			 ntohl(rule->key.spi));
		break;
	default:
		break;
	}
}

static bool gve_flow_rule_is_dup_rule(struct gve_priv *priv, struct gve_flow_rule *rule)
{
	struct gve_flow_rule *tmp;

	list_for_each_entry(tmp, &priv->flow_rules, list) {
		if (tmp->flow_type != rule->flow_type)
			continue;

		if (!memcmp(&tmp->key, &rule->key,
			    sizeof(struct gve_flow_spec)) &&
		    !memcmp(&tmp->mask, &rule->mask,
			    sizeof(struct gve_flow_spec)))
			return true;
	}
	return false;
}

static struct gve_flow_rule *gve_find_flow_rule_by_loc(struct gve_priv *priv, u16 loc)
{
	struct gve_flow_rule *rule;

	list_for_each_entry(rule, &priv->flow_rules, list)
		if (rule->loc == loc)
			return rule;

	return NULL;
}

static void gve_flow_rules_add_rule(struct gve_priv *priv, struct gve_flow_rule *rule)
{
	struct gve_flow_rule *tmp, *parent = NULL;

	list_for_each_entry(tmp, &priv->flow_rules, list) {
		if (tmp->loc >= rule->loc)
			break;
		parent = tmp;
	}

	if (parent)
		list_add(&rule->list, &parent->list);
	else
		list_add(&rule->list, &priv->flow_rules);

	priv->flow_rules_cnt++;
}

static void gve_flow_rules_del_rule(struct gve_priv *priv, struct gve_flow_rule *rule)
{
	list_del(&rule->list);
	kvfree(rule);
	priv->flow_rules_cnt--;
}

static int
gve_get_flow_rule_entry(struct gve_priv *priv, struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp = (struct ethtool_rx_flow_spec *)&cmd->fs;
	struct gve_flow_rule *rule = NULL;
	int err = 0;

	if (priv->flow_rules_max == 0)
		return -EOPNOTSUPP;

	spin_lock_bh(&priv->flow_rules_lock);
	rule = gve_find_flow_rule_by_loc(priv, fsp->location);
	if (!rule) {
		err = -EINVAL;
		goto ret;
	}

	switch (rule->flow_type) {
	case GVE_FLOW_TYPE_TCPV4:
		fsp->flow_type = TCP_V4_FLOW;
		break;
	case GVE_FLOW_TYPE_UDPV4:
		fsp->flow_type = UDP_V4_FLOW;
		break;
	case GVE_FLOW_TYPE_SCTPV4:
		fsp->flow_type = SCTP_V4_FLOW;
		break;
	case GVE_FLOW_TYPE_AHV4:
		fsp->flow_type = AH_V4_FLOW;
		break;
	case GVE_FLOW_TYPE_ESPV4:
		fsp->flow_type = ESP_V4_FLOW;
		break;
	case GVE_FLOW_TYPE_TCPV6:
		fsp->flow_type = TCP_V6_FLOW;
		break;
	case GVE_FLOW_TYPE_UDPV6:
		fsp->flow_type = UDP_V6_FLOW;
		break;
	case GVE_FLOW_TYPE_SCTPV6:
		fsp->flow_type = SCTP_V6_FLOW;
		break;
	case GVE_FLOW_TYPE_AHV6:
		fsp->flow_type = AH_V6_FLOW;
		break;
	case GVE_FLOW_TYPE_ESPV6:
		fsp->flow_type = ESP_V6_FLOW;
		break;
	default:
		err = -EINVAL;
		goto ret;
	}

	memset(&fsp->h_u, 0, sizeof(fsp->h_u));
	memset(&fsp->h_ext, 0, sizeof(fsp->h_ext));
	memset(&fsp->m_u, 0, sizeof(fsp->m_u));
	memset(&fsp->m_ext, 0, sizeof(fsp->m_ext));

	switch (fsp->flow_type) {
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
	case SCTP_V4_FLOW:
		fsp->h_u.tcp_ip4_spec.ip4src = rule->key.src_ip[0];
		fsp->h_u.tcp_ip4_spec.ip4dst = rule->key.dst_ip[0];
		fsp->h_u.tcp_ip4_spec.psrc = rule->key.src_port;
		fsp->h_u.tcp_ip4_spec.pdst = rule->key.dst_port;
		fsp->h_u.tcp_ip4_spec.tos = rule->key.tos;
		fsp->m_u.tcp_ip4_spec.ip4src = rule->mask.src_ip[0];
		fsp->m_u.tcp_ip4_spec.ip4dst = rule->mask.dst_ip[0];
		fsp->m_u.tcp_ip4_spec.psrc = rule->mask.src_port;
		fsp->m_u.tcp_ip4_spec.pdst = rule->mask.dst_port;
		fsp->m_u.tcp_ip4_spec.tos = rule->mask.tos;
		break;
	case AH_V4_FLOW:
	case ESP_V4_FLOW:
		fsp->h_u.ah_ip4_spec.ip4src = rule->key.src_ip[0];
		fsp->h_u.ah_ip4_spec.ip4dst = rule->key.dst_ip[0];
		fsp->h_u.ah_ip4_spec.spi = rule->key.spi;
		fsp->h_u.ah_ip4_spec.tos = rule->key.tos;
		fsp->m_u.ah_ip4_spec.ip4src = rule->mask.src_ip[0];
		fsp->m_u.ah_ip4_spec.ip4dst = rule->mask.dst_ip[0];
		fsp->m_u.ah_ip4_spec.spi = rule->mask.spi;
		fsp->m_u.ah_ip4_spec.tos = rule->mask.tos;
		break;
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
	case SCTP_V6_FLOW:
		memcpy(fsp->h_u.tcp_ip6_spec.ip6src, &rule->key.src_ip,
		       sizeof(struct in6_addr));
		memcpy(fsp->h_u.tcp_ip6_spec.ip6dst, &rule->key.dst_ip,
		       sizeof(struct in6_addr));
		fsp->h_u.tcp_ip6_spec.psrc = rule->key.src_port;
		fsp->h_u.tcp_ip6_spec.pdst = rule->key.dst_port;
		fsp->h_u.tcp_ip6_spec.tclass = rule->key.tclass;
		memcpy(fsp->m_u.tcp_ip6_spec.ip6src, &rule->mask.src_ip,
		       sizeof(struct in6_addr));
		memcpy(fsp->m_u.tcp_ip6_spec.ip6dst, &rule->mask.dst_ip,
		       sizeof(struct in6_addr));
		fsp->m_u.tcp_ip6_spec.psrc = rule->mask.src_port;
		fsp->m_u.tcp_ip6_spec.pdst = rule->mask.dst_port;
		fsp->m_u.tcp_ip6_spec.tclass = rule->mask.tclass;
		break;
	case AH_V6_FLOW:
	case ESP_V6_FLOW:
		memcpy(fsp->h_u.ah_ip6_spec.ip6src, &rule->key.src_ip,
		       sizeof(struct in6_addr));
		memcpy(fsp->h_u.ah_ip6_spec.ip6dst, &rule->key.dst_ip,
		       sizeof(struct in6_addr));
		fsp->h_u.ah_ip6_spec.spi = rule->key.spi;
		fsp->h_u.ah_ip6_spec.tclass = rule->key.tclass;
		memcpy(fsp->m_u.ah_ip6_spec.ip6src, &rule->mask.src_ip,
		       sizeof(struct in6_addr));
		memcpy(fsp->m_u.ah_ip6_spec.ip6dst, &rule->mask.dst_ip,
		       sizeof(struct in6_addr));
		fsp->m_u.ah_ip6_spec.spi = rule->mask.spi;
		fsp->m_u.ah_ip6_spec.tclass = rule->mask.tclass;
		break;
	default:
		err = -EINVAL;
		goto ret;
	}

	fsp->ring_cookie = rule->action;

ret:
	spin_unlock_bh(&priv->flow_rules_lock);
	return err;
}

static int
gve_get_flow_rule_ids(struct gve_priv *priv, struct ethtool_rxnfc *cmd,
		      u32 *rule_locs)
{
	struct gve_flow_rule *rule;
	unsigned int cnt = 0;
	int err = 0;

	if (priv->flow_rules_max == 0)
		return -EOPNOTSUPP;

	cmd->data = priv->flow_rules_max;

	spin_lock_bh(&priv->flow_rules_lock);
	list_for_each_entry(rule, &priv->flow_rules, list) {
		if (cnt == cmd->rule_cnt) {
			err = -EMSGSIZE;
			goto ret;
		}
		rule_locs[cnt] = rule->loc;
		cnt++;
	}
	cmd->rule_cnt = cnt;

ret:
	spin_unlock_bh(&priv->flow_rules_lock);
	return err;
}

static int
gve_add_flow_rule_info(struct gve_priv *priv, struct ethtool_rx_flow_spec *fsp,
		       struct gve_flow_rule *rule)
{
	u32 flow_type, q_index = 0;

	if (fsp->ring_cookie == RX_CLS_FLOW_DISC)
		return -EOPNOTSUPP;

	q_index = fsp->ring_cookie;
	if (q_index >= priv->rx_cfg.num_queues)
		return -EINVAL;

	rule->action = q_index;
	rule->loc = fsp->location;

	flow_type = fsp->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT | FLOW_RSS);
	switch (flow_type) {
	case TCP_V4_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_TCPV4;
		break;
	case UDP_V4_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_UDPV4;
		break;
	case SCTP_V4_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_SCTPV4;
		break;
	case AH_V4_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_AHV4;
		break;
	case ESP_V4_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_ESPV4;
		break;
	case TCP_V6_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_TCPV6;
		break;
	case UDP_V6_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_UDPV6;
		break;
	case SCTP_V6_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_SCTPV6;
		break;
	case AH_V6_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_AHV6;
		break;
	case ESP_V6_FLOW:
		rule->flow_type = GVE_FLOW_TYPE_ESPV6;
		break;
	default:
		return -EINVAL;
	}

	switch (flow_type) {
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
	case SCTP_V4_FLOW:
		rule->key.src_ip[0] = fsp->h_u.tcp_ip4_spec.ip4src;
		rule->key.dst_ip[0] = fsp->h_u.tcp_ip4_spec.ip4dst;
		rule->key.src_port = fsp->h_u.tcp_ip4_spec.psrc;
		rule->key.dst_port = fsp->h_u.tcp_ip4_spec.pdst;
		rule->mask.src_ip[0] = fsp->m_u.tcp_ip4_spec.ip4src;
		rule->mask.dst_ip[0] = fsp->m_u.tcp_ip4_spec.ip4dst;
		rule->mask.src_port = fsp->m_u.tcp_ip4_spec.psrc;
		rule->mask.dst_port = fsp->m_u.tcp_ip4_spec.pdst;
		break;
	case AH_V4_FLOW:
	case ESP_V4_FLOW:
		rule->key.src_ip[0] = fsp->h_u.tcp_ip4_spec.ip4src;
		rule->key.dst_ip[0] = fsp->h_u.tcp_ip4_spec.ip4dst;
		rule->key.spi = fsp->h_u.ah_ip4_spec.spi;
		rule->mask.src_ip[0] = fsp->m_u.tcp_ip4_spec.ip4src;
		rule->mask.dst_ip[0] = fsp->m_u.tcp_ip4_spec.ip4dst;
		rule->mask.spi = fsp->m_u.ah_ip4_spec.spi;
		break;
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
	case SCTP_V6_FLOW:
		memcpy(&rule->key.src_ip, fsp->h_u.tcp_ip6_spec.ip6src,
		       sizeof(struct in6_addr));
		memcpy(&rule->key.dst_ip, fsp->h_u.tcp_ip6_spec.ip6dst,
		       sizeof(struct in6_addr));
		rule->key.src_port = fsp->h_u.tcp_ip6_spec.psrc;
		rule->key.dst_port = fsp->h_u.tcp_ip6_spec.pdst;
		memcpy(&rule->mask.src_ip, fsp->m_u.tcp_ip6_spec.ip6src,
		       sizeof(struct in6_addr));
		memcpy(&rule->mask.dst_ip, fsp->m_u.tcp_ip6_spec.ip6dst,
		       sizeof(struct in6_addr));
		rule->mask.src_port = fsp->m_u.tcp_ip6_spec.psrc;
		rule->mask.dst_port = fsp->m_u.tcp_ip6_spec.pdst;
		break;
	case AH_V6_FLOW:
	case ESP_V6_FLOW:
		memcpy(&rule->key.src_ip, fsp->h_u.usr_ip6_spec.ip6src,
		       sizeof(struct in6_addr));
		memcpy(&rule->key.dst_ip, fsp->h_u.usr_ip6_spec.ip6dst,
		       sizeof(struct in6_addr));
		rule->key.spi = fsp->h_u.ah_ip6_spec.spi;
		memcpy(&rule->mask.src_ip, fsp->m_u.usr_ip6_spec.ip6src,
		       sizeof(struct in6_addr));
		memcpy(&rule->mask.dst_ip, fsp->m_u.usr_ip6_spec.ip6dst,
		       sizeof(struct in6_addr));
		rule->key.spi = fsp->h_u.ah_ip6_spec.spi;
		break;
	default:
		/* not doing un-parsed flow types */
		return -EINVAL;
	}

	if (gve_flow_rule_is_dup_rule(priv, rule))
		return -EEXIST;

	return 0;
}

static int gve_add_flow_rule(struct gve_priv *priv, struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp = &cmd->fs;
	struct gve_flow_rule *rule = NULL;
	int err;

	if (priv->flow_rules_max == 0)
		return -EOPNOTSUPP;

	if (priv->flow_rules_cnt >= priv->flow_rules_max) {
		dev_err(&priv->pdev->dev,
			"Reached the limit of max allowed flow rules (%u)\n",
			priv->flow_rules_max);
		return -ENOSPC;
	}

	spin_lock_bh(&priv->flow_rules_lock);
	if (gve_find_flow_rule_by_loc(priv, fsp->location)) {
		dev_err(&priv->pdev->dev, "Flow rule %d already exists\n",
			fsp->location);
		err = -EEXIST;
		goto ret;
	}

	rule = kvzalloc(sizeof(*rule), GFP_KERNEL);
	if (!rule) {
		err = -ENOMEM;
		goto ret;
	}

	err = gve_add_flow_rule_info(priv, fsp, rule);
	if (err)
		goto ret;

	err = gve_adminq_add_flow_rule(priv, rule);
	if (err)
		goto ret;

	gve_flow_rules_add_rule(priv, rule);
	gve_print_flow_rule(priv, rule);

ret:
	spin_unlock_bh(&priv->flow_rules_lock);
	if (err && rule)
		kfree(rule);
	return err;
}

static int gve_del_flow_rule(struct gve_priv *priv, struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp = (struct ethtool_rx_flow_spec *)&cmd->fs;
	struct gve_flow_rule *rule = NULL;
	int err = 0;

	if (priv->flow_rules_max == 0)
		return -EOPNOTSUPP;

	spin_lock_bh(&priv->flow_rules_lock);
	rule = gve_find_flow_rule_by_loc(priv, fsp->location);
	if (!rule) {
		err = -EINVAL;
		goto ret;
	}

	err = gve_adminq_del_flow_rule(priv, fsp->location);
	if (err)
		goto ret;

	gve_flow_rules_del_rule(priv, rule);

ret:
	spin_unlock_bh(&priv->flow_rules_lock);
	return err;
}

static int gve_set_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);
	int err = -EOPNOTSUPP;

	if (!(netdev->features & NETIF_F_NTUPLE))
		return err;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		err = gve_add_flow_rule(priv, cmd);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		err = gve_del_flow_rule(priv, cmd);
		break;
	case ETHTOOL_SRXFH:
		/* not supported */
		break;
	default:
		break;
	}

	return err;
}

static int gve_get_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd,
			 u32 *rule_locs)
{
	struct gve_priv *priv = netdev_priv(netdev);
	int err = -EOPNOTSUPP;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = priv->rx_cfg.num_queues;
		err = 0;
		break;
	case ETHTOOL_GRXCLSRLCNT:
		if (priv->flow_rules_max == 0)
			break;
		cmd->rule_cnt = priv->flow_rules_cnt;
		cmd->data = priv->flow_rules_max;
		err = 0;
		break;
	case ETHTOOL_GRXCLSRULE:
		err = gve_get_flow_rule_entry(priv, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		err = gve_get_flow_rule_ids(priv, cmd, (u32 *)rule_locs);
		break;
	case ETHTOOL_GRXFH:
		/* not supported */
		break;
	default:
		break;
	}

	return err;
}

const struct ethtool_ops gve_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS,
	.get_drvinfo = gve_get_drvinfo,
	.get_strings = gve_get_strings,
	.get_sset_count = gve_get_sset_count,
	.get_ethtool_stats = gve_get_ethtool_stats,
	.set_msglevel = gve_set_msglevel,
	.get_msglevel = gve_get_msglevel,
	.set_channels = gve_set_channels,
	.get_channels = gve_get_channels,
	.set_rxnfc = gve_set_rxnfc,
	.get_rxnfc = gve_get_rxnfc,
	.get_rxfh_indir_size = gve_get_rxfh_indir_size,
	.get_rxfh_key_size = gve_get_rxfh_key_size,
	.get_rxfh = gve_get_rxfh,
	.set_rxfh = gve_set_rxfh,
	.get_link = ethtool_op_get_link,
	.get_coalesce = gve_get_coalesce,
	.set_coalesce = gve_set_coalesce,
	.get_ringparam = gve_get_ringparam,
	.set_ringparam = gve_set_ringparam,
	.reset = gve_user_reset,
	.get_tunable = gve_get_tunable,
	.set_tunable = gve_set_tunable,
	.get_priv_flags = gve_get_priv_flags,
	.set_priv_flags = gve_set_priv_flags,
	.get_link_ksettings = gve_get_link_ksettings
};
