// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2021 Google, Inc.
 */

#include <linux/etherdevice.h>
#include <linux/pci.h>
#include "gve.h"
#include "gve_adminq.h"
#include "gve_register.h"

#define GVE_MAX_ADMINQ_RELEASE_CHECK	500
#define GVE_ADMINQ_SLEEP_LEN		20
#define GVE_MAX_ADMINQ_EVENT_COUNTER_CHECK	100

#define GVE_DEVICE_OPTION_ERROR_FMT "%s option error:\n" \
"Expected: length=%d, feature_mask=%x.\n" \
"Actual: length=%d, feature_mask=%x.\n"

#define GVE_DEVICE_OPTION_TOO_BIG_FMT "Length of %s option larger than expected. Possible older version of guest driver.\n"

static
struct gve_device_option *gve_get_next_option(struct gve_device_descriptor *descriptor,
					      struct gve_device_option *option)
{
	void *option_end, *descriptor_end;

	option_end = (void *)(option + 1) + be16_to_cpu(option->option_length);
	descriptor_end = (void *)descriptor + be16_to_cpu(descriptor->total_length);

	return option_end > descriptor_end ? NULL : (struct gve_device_option *)option_end;
}

static
void gve_parse_device_option(struct gve_priv *priv,
			     struct gve_device_descriptor *device_descriptor,
			     struct gve_device_option *option,
			     struct gve_device_option_gqi_rda **dev_op_gqi_rda,
			     struct gve_device_option_gqi_qpl **dev_op_gqi_qpl,
			     struct gve_device_option_dqo_rda **dev_op_dqo_rda,
			     struct gve_device_option_jumbo_frames **dev_op_jumbo_frames,
			     struct gve_device_option_buffer_sizes **dev_op_buffer_sizes,
			     struct gve_device_option_flow_steering **dev_op_flow_steering,
			     struct gve_device_option_dqo_qpl **dev_op_dqo_qpl)
{
	u32 req_feat_mask = be32_to_cpu(option->required_features_mask);
	u16 option_length = be16_to_cpu(option->option_length);
	u16 option_id = be16_to_cpu(option->option_id);

	/* If the length or feature mask doesn't match, continue without
	 * enabling the feature.
	 */
	switch (option_id) {
	case GVE_DEV_OPT_ID_GQI_RAW_ADDRESSING:
		if (option_length != GVE_DEV_OPT_LEN_GQI_RAW_ADDRESSING ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_GQI_RAW_ADDRESSING) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "Raw Addressing",
				 GVE_DEV_OPT_LEN_GQI_RAW_ADDRESSING,
				 GVE_DEV_OPT_REQ_FEAT_MASK_GQI_RAW_ADDRESSING,
				 option_length, req_feat_mask);
			break;
		}

		dev_info(&priv->pdev->dev,
			 "Gqi raw addressing device option enabled.\n");
		priv->queue_format = GVE_GQI_RDA_FORMAT;
		break;
	case GVE_DEV_OPT_ID_GQI_RDA:
		if (option_length < sizeof(**dev_op_gqi_rda) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_GQI_RDA) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "GQI RDA", (int)sizeof(**dev_op_gqi_rda),
				 GVE_DEV_OPT_REQ_FEAT_MASK_GQI_RDA,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_gqi_rda)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT, "GQI RDA");
		}
		*dev_op_gqi_rda = (void *)(option + 1);
		break;
	case GVE_DEV_OPT_ID_GQI_QPL:
		if (option_length < sizeof(**dev_op_gqi_qpl) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_GQI_QPL) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "GQI QPL", (int)sizeof(**dev_op_gqi_qpl),
				 GVE_DEV_OPT_REQ_FEAT_MASK_GQI_QPL,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_gqi_qpl)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT, "GQI QPL");
		}
		*dev_op_gqi_qpl = (void *)(option + 1);
		break;
	case GVE_DEV_OPT_ID_DQO_RDA:
		if (option_length < sizeof(**dev_op_dqo_rda) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_DQO_RDA) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "DQO RDA", (int)sizeof(**dev_op_dqo_rda),
				 GVE_DEV_OPT_REQ_FEAT_MASK_DQO_RDA,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_dqo_rda)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT, "DQO RDA");
		}
		*dev_op_dqo_rda = (void *)(option + 1);
		break;
	case GVE_DEV_OPT_ID_DQO_QPL:
		if (option_length < sizeof(**dev_op_dqo_qpl) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_DQO_QPL) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "DQO QPL", (int)sizeof(**dev_op_dqo_qpl),
				 GVE_DEV_OPT_REQ_FEAT_MASK_DQO_QPL,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_dqo_qpl)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT, "DQO QPL");
		}
		*dev_op_dqo_qpl = (void *)(option + 1);
		break;
	case GVE_DEV_OPT_ID_JUMBO_FRAMES:
		if (option_length < sizeof(**dev_op_jumbo_frames) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_JUMBO_FRAMES) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "Jumbo Frames",
				 (int)sizeof(**dev_op_jumbo_frames),
				 GVE_DEV_OPT_REQ_FEAT_MASK_JUMBO_FRAMES,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_jumbo_frames)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT,
				 "Jumbo Frames");
		}
		*dev_op_jumbo_frames = (void *)(option + 1);
		break;
	case GVE_DEV_OPT_ID_BUFFER_SIZES:
		if (option_length < sizeof(**dev_op_buffer_sizes) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_BUFFER_SIZES) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "Buffer Sizes",
				 (int)sizeof(**dev_op_buffer_sizes),
				 GVE_DEV_OPT_REQ_FEAT_MASK_BUFFER_SIZES,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_buffer_sizes)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT,
				 "Buffer Sizes");
		}
		*dev_op_buffer_sizes = (void *)(option + 1);
		if ((*dev_op_buffer_sizes)->header_buffer_size)
			priv->ethtool_defaults |= BIT(GVE_PRIV_FLAGS_ENABLE_HEADER_SPLIT);
		break;
	case GVE_DEV_OPT_ID_FLOW_STEERING:
		if (option_length < sizeof(**dev_op_flow_steering) ||
		    req_feat_mask != GVE_DEV_OPT_REQ_FEAT_MASK_FLOW_STEERING) {
			dev_warn(&priv->pdev->dev, GVE_DEVICE_OPTION_ERROR_FMT,
				 "Flow Steering",
				 (int)sizeof(**dev_op_flow_steering),
				 GVE_DEV_OPT_REQ_FEAT_MASK_FLOW_STEERING,
				 option_length, req_feat_mask);
			break;
		}

		if (option_length > sizeof(**dev_op_flow_steering)) {
			dev_warn(&priv->pdev->dev,
				 GVE_DEVICE_OPTION_TOO_BIG_FMT,
				 "Flow Steering");
		}
		*dev_op_flow_steering = (void *)(option + 1);
		break;
	default:
		/* If we don't recognize the option just continue
		 * without doing anything.
		 */
		dev_dbg(&priv->pdev->dev, "Unrecognized device option 0x%hx not enabled.\n",
			option_id);
	}
}

/* Process all device options for a given describe device call. */
static int
gve_process_device_options(struct gve_priv *priv,
			   struct gve_device_descriptor *descriptor,
			   struct gve_device_option_gqi_rda **dev_op_gqi_rda,
			   struct gve_device_option_gqi_qpl **dev_op_gqi_qpl,
			   struct gve_device_option_dqo_rda **dev_op_dqo_rda,
			   struct gve_device_option_jumbo_frames **dev_op_jumbo_frames,
			   struct gve_device_option_buffer_sizes **dev_op_buffer_sizes,
			   struct gve_device_option_flow_steering **dev_op_flow_steering,
			   struct gve_device_option_dqo_qpl **dev_op_dqo_qpl)
{
	const int num_options = be16_to_cpu(descriptor->num_device_options);
	struct gve_device_option *dev_opt;
	int i;

	/* The options struct directly follows the device descriptor. */
	dev_opt = (void *)(descriptor + 1);
	for (i = 0; i < num_options; i++) {
		struct gve_device_option *next_opt;

		next_opt = gve_get_next_option(descriptor, dev_opt);
		if (!next_opt) {
			dev_err(&priv->dev->dev,
				"options exceed device_descriptor's total length.\n");
			return -EINVAL;
		}

		gve_parse_device_option(priv, descriptor, dev_opt,
					dev_op_gqi_rda, dev_op_gqi_qpl,
					dev_op_dqo_rda, dev_op_jumbo_frames,
					dev_op_buffer_sizes, dev_op_flow_steering,
					dev_op_dqo_qpl);
		dev_opt = next_opt;
	}

	return 0;
}

int gve_adminq_alloc(struct device *dev, struct gve_priv *priv)
{
	priv->adminq = dma_alloc_coherent(dev, PAGE_SIZE,
					  &priv->adminq_bus_addr, GFP_KERNEL);
	if (unlikely(!priv->adminq))
		return -ENOMEM;

	priv->adminq_mask = (PAGE_SIZE / sizeof(union gve_adminq_command)) - 1;
	priv->adminq_prod_cnt = 0;
	priv->adminq_cmd_fail = 0;
	priv->adminq_timeouts = 0;
	priv->adminq_describe_device_cnt = 0;
	priv->adminq_cfg_device_resources_cnt = 0;
	priv->adminq_register_page_list_cnt = 0;
	priv->adminq_unregister_page_list_cnt = 0;
	priv->adminq_create_tx_queue_cnt = 0;
	priv->adminq_create_rx_queue_cnt = 0;
	priv->adminq_destroy_tx_queue_cnt = 0;
	priv->adminq_destroy_rx_queue_cnt = 0;
	priv->adminq_dcfg_device_resources_cnt = 0;
	priv->adminq_set_driver_parameter_cnt = 0;
	priv->adminq_report_stats_cnt = 0;
	priv->adminq_report_link_speed_cnt = 0;
	priv->adminq_get_ptype_map_cnt = 0;
	priv->adminq_cfg_flow_rule_cnt = 0;

	/* Setup Admin queue with the device */
	iowrite32be(priv->adminq_bus_addr / PAGE_SIZE,
		    &priv->reg_bar0->adminq_pfn);

	gve_set_admin_queue_ok(priv);
	return 0;
}

void gve_adminq_release(struct gve_priv *priv)
{
	int i = 0;

	/* Tell the device the adminq is leaving */
	iowrite32be(0x0, &priv->reg_bar0->adminq_pfn);
	while (ioread32be(&priv->reg_bar0->adminq_pfn)) {
		/* If this is reached the device is unrecoverable and still
		 * holding memory. Continue looping to avoid memory corruption,
		 * but WARN so it is visible what is going on.
		 */
		if (i == GVE_MAX_ADMINQ_RELEASE_CHECK)
			WARN(1, "Unrecoverable platform error!");
		i++;
		msleep(GVE_ADMINQ_SLEEP_LEN);
	}
	gve_clear_device_rings_ok(priv);
	gve_clear_device_resources_ok(priv);
	gve_clear_admin_queue_ok(priv);
}

void gve_adminq_free(struct device *dev, struct gve_priv *priv)
{
	if (!gve_get_admin_queue_ok(priv))
		return;
	gve_adminq_release(priv);
	dma_free_coherent(dev, PAGE_SIZE, priv->adminq, priv->adminq_bus_addr);
	gve_clear_admin_queue_ok(priv);
}

static void gve_adminq_kick_cmd(struct gve_priv *priv, u32 prod_cnt)
{
	iowrite32be(prod_cnt, &priv->reg_bar0->adminq_doorbell);
}

static bool gve_adminq_wait_for_cmd(struct gve_priv *priv, u32 prod_cnt)
{
	int i;

	for (i = 0; i < GVE_MAX_ADMINQ_EVENT_COUNTER_CHECK; i++) {
		if (ioread32be(&priv->reg_bar0->adminq_event_counter)
		    == prod_cnt)
			return true;
		msleep(GVE_ADMINQ_SLEEP_LEN);
	}

	return false;
}

static int gve_adminq_parse_err(struct gve_priv *priv, u32 status)
{
	if (status != GVE_ADMINQ_COMMAND_PASSED &&
	    status != GVE_ADMINQ_COMMAND_UNSET) {
		dev_err(&priv->pdev->dev, "AQ command failed with status %d\n", status);
		priv->adminq_cmd_fail++;
	}
	switch (status) {
	case GVE_ADMINQ_COMMAND_PASSED:
		return 0;
	case GVE_ADMINQ_COMMAND_UNSET:
		dev_err(&priv->pdev->dev, "parse_aq_err: err and status both unset, this should not be possible.\n");
		return -EINVAL;
	case GVE_ADMINQ_COMMAND_ERROR_ABORTED:
	case GVE_ADMINQ_COMMAND_ERROR_CANCELLED:
	case GVE_ADMINQ_COMMAND_ERROR_DATALOSS:
	case GVE_ADMINQ_COMMAND_ERROR_FAILED_PRECONDITION:
	case GVE_ADMINQ_COMMAND_ERROR_UNAVAILABLE:
		return -EAGAIN;
	case GVE_ADMINQ_COMMAND_ERROR_ALREADY_EXISTS:
	case GVE_ADMINQ_COMMAND_ERROR_INTERNAL_ERROR:
	case GVE_ADMINQ_COMMAND_ERROR_INVALID_ARGUMENT:
	case GVE_ADMINQ_COMMAND_ERROR_NOT_FOUND:
	case GVE_ADMINQ_COMMAND_ERROR_OUT_OF_RANGE:
	case GVE_ADMINQ_COMMAND_ERROR_UNKNOWN_ERROR:
		return -EINVAL;
	case GVE_ADMINQ_COMMAND_ERROR_DEADLINE_EXCEEDED:
		return -ETIME;
	case GVE_ADMINQ_COMMAND_ERROR_PERMISSION_DENIED:
	case GVE_ADMINQ_COMMAND_ERROR_UNAUTHENTICATED:
		return -EACCES;
	case GVE_ADMINQ_COMMAND_ERROR_RESOURCE_EXHAUSTED:
		return -ENOMEM;
	case GVE_ADMINQ_COMMAND_ERROR_UNIMPLEMENTED:
		return -EOPNOTSUPP;
	default:
		dev_err(&priv->pdev->dev, "parse_aq_err: unknown status code %d\n", status);
		return -EINVAL;
	}
}

/* Flushes all AQ commands currently queued and waits for them to complete.
 * If there are failures, it will return the first error.
 */
static int gve_adminq_kick_and_wait(struct gve_priv *priv)
{
	u32 tail, head;
	int i;

	tail = ioread32be(&priv->reg_bar0->adminq_event_counter);
	head = priv->adminq_prod_cnt;

	gve_adminq_kick_cmd(priv, head);
	if (!gve_adminq_wait_for_cmd(priv, head)) {
		dev_err(&priv->pdev->dev, "AQ commands timed out, need to reset AQ\n");
		priv->adminq_timeouts++;
		return -ENOTRECOVERABLE;
	}

	for (i = tail; i < head; i++) {
		union gve_adminq_command *cmd;
		u32 status, err;

		cmd = &priv->adminq[i & priv->adminq_mask];
		status = be32_to_cpu(READ_ONCE(cmd->status));
		err = gve_adminq_parse_err(priv, status);
		if (err)
			// Return the first error if we failed.
			return err;
	}

	return 0;
}

/* This function is not threadsafe - the caller is responsible for any
 * necessary locks.
 */
static int gve_adminq_issue_cmd(struct gve_priv *priv,
				union gve_adminq_command *cmd_orig)
{
	union gve_adminq_command *cmd;
	u32 opcode;
	u32 tail;

	tail = ioread32be(&priv->reg_bar0->adminq_event_counter);

	// Check if next command will overflow the buffer.
	if (((priv->adminq_prod_cnt + 1) & priv->adminq_mask) ==
	    (tail & priv->adminq_mask)) {
		int err;

		// Flush existing commands to make room.
		err = gve_adminq_kick_and_wait(priv);
		if (err)
			return err;

		// Retry.
		tail = ioread32be(&priv->reg_bar0->adminq_event_counter);
		if (((priv->adminq_prod_cnt + 1) & priv->adminq_mask) ==
		    (tail & priv->adminq_mask)) {
			// This should never happen. We just flushed the
			// command queue so there should be enough space.
			return -ENOMEM;
		}
	}

	cmd = &priv->adminq[priv->adminq_prod_cnt & priv->adminq_mask];
	priv->adminq_prod_cnt++;

	memcpy(cmd, cmd_orig, sizeof(*cmd_orig));
	opcode = be32_to_cpu(READ_ONCE(cmd->opcode));
	if (opcode == GVE_ADMINQ_EXTENDED_COMMAND)
		opcode = be32_to_cpu(cmd->extended_command.inner_opcode);

	switch (opcode) {
	case GVE_ADMINQ_DESCRIBE_DEVICE:
		priv->adminq_describe_device_cnt++;
		break;
	case GVE_ADMINQ_CONFIGURE_DEVICE_RESOURCES:
		priv->adminq_cfg_device_resources_cnt++;
		break;
	case GVE_ADMINQ_REGISTER_PAGE_LIST:
		priv->adminq_register_page_list_cnt++;
		break;
	case GVE_ADMINQ_UNREGISTER_PAGE_LIST:
		priv->adminq_unregister_page_list_cnt++;
		break;
	case GVE_ADMINQ_CREATE_TX_QUEUE:
		priv->adminq_create_tx_queue_cnt++;
		break;
	case GVE_ADMINQ_CREATE_RX_QUEUE:
		priv->adminq_create_rx_queue_cnt++;
		break;
	case GVE_ADMINQ_DESTROY_TX_QUEUE:
		priv->adminq_destroy_tx_queue_cnt++;
		break;
	case GVE_ADMINQ_DESTROY_RX_QUEUE:
		priv->adminq_destroy_rx_queue_cnt++;
		break;
	case GVE_ADMINQ_DECONFIGURE_DEVICE_RESOURCES:
		priv->adminq_dcfg_device_resources_cnt++;
		break;
	case GVE_ADMINQ_CONFIGURE_RSS:
		priv->adminq_cfg_rss_cnt++;
		break;
	case GVE_ADMINQ_SET_DRIVER_PARAMETER:
		priv->adminq_set_driver_parameter_cnt++;
		break;
	case GVE_ADMINQ_REPORT_STATS:
		priv->adminq_report_stats_cnt++;
		break;
	case GVE_ADMINQ_REPORT_LINK_SPEED:
		priv->adminq_report_link_speed_cnt++;
		break;
	case GVE_ADMINQ_GET_PTYPE_MAP:
		priv->adminq_get_ptype_map_cnt++;
		break;
	case GVE_ADMINQ_VERIFY_DRIVER_COMPATIBILITY:
		priv->adminq_verify_driver_compatibility_cnt++;
		break;
	case GVE_ADMINQ_CONFIGURE_FLOW_RULE:
		priv->adminq_cfg_flow_rule_cnt++;
		break;
	default:
		dev_err(&priv->pdev->dev, "unknown AQ command opcode %d\n", opcode);
	}

	return 0;
}

/* This function is not threadsafe - the caller is responsible for any
 * necessary locks.
 * The caller is also responsible for making sure there are no commands
 * waiting to be executed.
 */
static int gve_adminq_execute_cmd(struct gve_priv *priv,
				  union gve_adminq_command *cmd_orig)
{
	u32 tail, head;
	int err;

	tail = ioread32be(&priv->reg_bar0->adminq_event_counter);
	head = priv->adminq_prod_cnt;
	if (tail != head)
		// This is not a valid path
		return -EINVAL;

	err = gve_adminq_issue_cmd(priv, cmd_orig);
	if (err)
		return err;

	return gve_adminq_kick_and_wait(priv);
}

static int gve_adminq_execute_extended_cmd(struct gve_priv *priv,
					   uint32_t opcode, size_t cmd_size,
					   void *cmd_orig)
{
	union gve_adminq_command cmd;
	dma_addr_t inner_cmd_bus;
	void *inner_cmd;
	int err;

	inner_cmd = dma_alloc_coherent(&priv->pdev->dev, cmd_size,
				       &inner_cmd_bus, GFP_KERNEL);
	if (!inner_cmd)
		return -ENOMEM;

	memcpy(inner_cmd, cmd_orig, cmd_size);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_EXTENDED_COMMAND);
	cmd.extended_command = (struct gve_adminq_extended_command) {
		.inner_opcode = cpu_to_be32(opcode),
		.inner_length = cpu_to_be32(cmd_size),
		.inner_command_addr = cpu_to_be64(inner_cmd_bus),
	};

	err = gve_adminq_execute_cmd(priv, &cmd);

	dma_free_coherent(&priv->pdev->dev,
			  cmd_size,
			  inner_cmd, inner_cmd_bus);
	return err;
}


/* The device specifies that the management vector can either be the first irq
 * or the last irq. ntfy_blk_msix_base_idx indicates the first irq assigned to
 * the ntfy blks. It if is 0 then the management vector is last, if it is 1 then
 * the management vector is first.
 *
 * gve arranges the msix vectors so that the management vector is last.
 */
#define GVE_NTFY_BLK_BASE_MSIX_IDX	0
int gve_adminq_configure_device_resources(struct gve_priv *priv,
					  dma_addr_t counter_array_bus_addr,
					  u32 num_counters,
					  dma_addr_t db_array_bus_addr,
					  u32 num_ntfy_blks)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CONFIGURE_DEVICE_RESOURCES);
	cmd.configure_device_resources =
		(struct gve_adminq_configure_device_resources) {
		.counter_array = cpu_to_be64(counter_array_bus_addr),
		.num_counters = cpu_to_be32(num_counters),
		.irq_db_addr = cpu_to_be64(db_array_bus_addr),
		.num_irq_dbs = cpu_to_be32(num_ntfy_blks),
		.irq_db_stride = cpu_to_be32(sizeof(*priv->irq_db_indices)),
		.ntfy_blk_msix_base_idx =
					cpu_to_be32(GVE_NTFY_BLK_BASE_MSIX_IDX),
		.queue_format = priv->queue_format,
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_deconfigure_device_resources(struct gve_priv *priv)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DECONFIGURE_DEVICE_RESOURCES);

	return gve_adminq_execute_cmd(priv, &cmd);
}

static int gve_adminq_create_tx_queue(struct gve_priv *priv, u32 queue_index)
{
	struct gve_tx_ring *tx = &priv->tx[queue_index];
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CREATE_TX_QUEUE);
	cmd.create_tx_queue = (struct gve_adminq_create_tx_queue) {
		.queue_id = cpu_to_be32(queue_index),
		.queue_resources_addr =
			cpu_to_be64(tx->q_resources_bus),
		.tx_ring_addr = cpu_to_be64(tx->bus),
		.ntfy_id = cpu_to_be32(tx->ntfy_id),
	};

	if (gve_is_gqi(priv)) {
		u32 qpl_id = priv->queue_format == GVE_GQI_RDA_FORMAT ?
			GVE_RAW_ADDRESSING_QPL_ID : tx->tx_fifo.qpl->id;

		cmd.create_tx_queue.queue_page_list_id = cpu_to_be32(qpl_id);
	} else {
		u16 comp_ring_size;
		u32 qpl_id = 0;

		if (priv->queue_format == GVE_DQO_RDA_FORMAT) {
			qpl_id = GVE_RAW_ADDRESSING_QPL_ID;
			comp_ring_size =
				priv->options_dqo_rda.tx_comp_ring_entries;
		} else {
			qpl_id = tx->dqo.qpl->id;
			comp_ring_size = priv->tx_desc_cnt;
		}
		cmd.create_tx_queue.queue_page_list_id = cpu_to_be32(qpl_id);
		cmd.create_tx_queue.tx_ring_size =
			cpu_to_be16(priv->tx_desc_cnt);
		cmd.create_tx_queue.tx_comp_ring_addr =
			cpu_to_be64(tx->complq_bus_dqo);
		cmd.create_tx_queue.tx_comp_ring_size =
			cpu_to_be16(comp_ring_size);
	}

	return gve_adminq_issue_cmd(priv, &cmd);
}

int gve_adminq_create_tx_queues(struct gve_priv *priv, u32 start_id, u32 num_queues)
{
	int err;
	int i;

	for (i = start_id; i < start_id + num_queues; i++) {
		err = gve_adminq_create_tx_queue(priv, i);
		if (err)
			return err;
	}

	return gve_adminq_kick_and_wait(priv);
}

static int gve_adminq_create_rx_queue(struct gve_priv *priv, u32 queue_index)
{
	struct gve_rx_ring *rx = &priv->rx[queue_index];
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CREATE_RX_QUEUE);
	cmd.create_rx_queue = (struct gve_adminq_create_rx_queue) {
		.queue_id = cpu_to_be32(queue_index),
		.ntfy_id = cpu_to_be32(rx->ntfy_id),
		.queue_resources_addr = cpu_to_be64(rx->q_resources_bus),
	};

	if (gve_is_gqi(priv)) {
		u32 qpl_id = priv->queue_format == GVE_GQI_RDA_FORMAT ?
			GVE_RAW_ADDRESSING_QPL_ID : rx->data.qpl->id;

		cmd.create_rx_queue.rx_desc_ring_addr =
			cpu_to_be64(rx->desc.bus),
		cmd.create_rx_queue.rx_data_ring_addr =
			cpu_to_be64(rx->data.data_bus),
		cmd.create_rx_queue.index = cpu_to_be32(queue_index);
		cmd.create_rx_queue.queue_page_list_id = cpu_to_be32(qpl_id);
		cmd.create_rx_queue.packet_buffer_size = cpu_to_be16(rx->packet_buffer_size);
	} else {
		u16 rx_buff_ring_entries;
		u32 qpl_id = 0;

		if (priv->queue_format == GVE_DQO_RDA_FORMAT) {
			qpl_id = GVE_RAW_ADDRESSING_QPL_ID;
			rx_buff_ring_entries =
				priv->options_dqo_rda.rx_buff_ring_entries;
		} else {
			qpl_id = rx->dqo.qpl->id;
			rx_buff_ring_entries = priv->rx_desc_cnt;
		}
		cmd.create_rx_queue.queue_page_list_id = cpu_to_be32(qpl_id);
		cmd.create_rx_queue.rx_ring_size =
			cpu_to_be16(priv->rx_desc_cnt);
		cmd.create_rx_queue.rx_desc_ring_addr =
			cpu_to_be64(rx->dqo.complq.bus);
		cmd.create_rx_queue.rx_data_ring_addr =
			cpu_to_be64(rx->dqo.bufq.bus);
		cmd.create_rx_queue.packet_buffer_size =
			cpu_to_be16(priv->data_buffer_size_dqo);
		cmd.create_rx_queue.rx_buff_ring_size =
			cpu_to_be16(rx_buff_ring_entries);
		cmd.create_rx_queue.enable_rsc =
			!!(priv->dev->features & NETIF_F_LRO);
		if (gve_get_enable_header_split(priv))
			cmd.create_rx_queue.header_buffer_size =
				cpu_to_be16(priv->header_buf_size);
	}

	return gve_adminq_issue_cmd(priv, &cmd);
}

int gve_adminq_create_rx_queues(struct gve_priv *priv, u32 num_queues)
{
	int err;
	int i;

	for (i = 0; i < num_queues; i++) {
		err = gve_adminq_create_rx_queue(priv, i);
		if (err)
			return err;
	}

	return gve_adminq_kick_and_wait(priv);
}

static int gve_adminq_destroy_tx_queue(struct gve_priv *priv, u32 queue_index)
{
	union gve_adminq_command cmd;
	int err;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DESTROY_TX_QUEUE);
	cmd.destroy_tx_queue = (struct gve_adminq_destroy_tx_queue) {
		.queue_id = cpu_to_be32(queue_index),
	};

	err = gve_adminq_issue_cmd(priv, &cmd);
	if (err)
		return err;

	return 0;
}

int gve_adminq_destroy_tx_queues(struct gve_priv *priv, u32 start_id, u32 num_queues)
{
	int err;
	int i;

	for (i = start_id; i < start_id + num_queues; i++) {
		err = gve_adminq_destroy_tx_queue(priv, i);
		if (err)
			return err;
	}

	return gve_adminq_kick_and_wait(priv);
}

static int gve_adminq_destroy_rx_queue(struct gve_priv *priv, u32 queue_index)
{
	union gve_adminq_command cmd;
	int err;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DESTROY_RX_QUEUE);
	cmd.destroy_rx_queue = (struct gve_adminq_destroy_rx_queue) {
		.queue_id = cpu_to_be32(queue_index),
	};

	err = gve_adminq_issue_cmd(priv, &cmd);
	if (err)
		return err;

	return 0;
}

int gve_adminq_destroy_rx_queues(struct gve_priv *priv, u32 num_queues)
{
	int err;
	int i;

	for (i = 0; i < num_queues; i++) {
		err = gve_adminq_destroy_rx_queue(priv, i);
		if (err)
			return err;
	}

	return gve_adminq_kick_and_wait(priv);
}

static int gve_set_desc_cnt(struct gve_priv *priv,
			    struct gve_device_descriptor *descriptor)
{
	priv->tx_desc_cnt = be16_to_cpu(descriptor->tx_queue_entries);
	if (priv->tx_desc_cnt * sizeof(priv->tx->desc[0]) < PAGE_SIZE) {
		dev_err(&priv->pdev->dev, "Tx desc count %d too low\n",
			priv->tx_desc_cnt);
		return -EINVAL;
	}
	priv->rx_desc_cnt = be16_to_cpu(descriptor->rx_queue_entries);
	if (priv->rx_desc_cnt * sizeof(priv->rx->desc.desc_ring[0])
	    < PAGE_SIZE) {
		dev_err(&priv->pdev->dev, "Rx desc count %d too low\n",
			priv->rx_desc_cnt);
		return -EINVAL;
	}
	return 0;
}

static int
gve_set_desc_cnt_dqo(struct gve_priv *priv,
		     const struct gve_device_descriptor *descriptor,
		     const struct gve_device_option_dqo_rda *dev_op_dqo_rda)
{
	priv->tx_desc_cnt = be16_to_cpu(descriptor->tx_queue_entries);
	priv->rx_desc_cnt = be16_to_cpu(descriptor->rx_queue_entries);

	if (priv->queue_format == GVE_DQO_QPL_FORMAT)
		return 0;

	priv->options_dqo_rda.tx_comp_ring_entries =
		be16_to_cpu(dev_op_dqo_rda->tx_comp_ring_entries);
	priv->options_dqo_rda.rx_buff_ring_entries =
		be16_to_cpu(dev_op_dqo_rda->rx_buff_ring_entries);

	return 0;
}

static void gve_enable_supported_features(
	struct gve_priv *priv,
	u32 supported_features_mask,
	const struct gve_device_option_jumbo_frames *dev_op_jumbo_frames,
	const struct gve_device_option_buffer_sizes *dev_op_buffer_sizes,
	const struct gve_device_option_flow_steering *dev_op_flow_steering,
	const struct gve_device_option_dqo_qpl *dev_op_dqo_qpl)
{
	int buf_size;

	/* Before control reaches this point, the page-size-capped max MTU in
	 * the gve_device_descriptor field has already been stored in
	 * priv->dev->max_mtu. We overwrite it with the true max MTU below.
	 */
	if (dev_op_jumbo_frames &&
	    (supported_features_mask & GVE_SUP_JUMBO_FRAMES_MASK)) {
		dev_info(&priv->pdev->dev,
			 "JUMBO FRAMES device option enabled.\n");
		priv->dev->max_mtu = be16_to_cpu(dev_op_jumbo_frames->max_mtu);
	}

	priv->data_buffer_size_dqo = GVE_RX_BUFFER_SIZE_DQO;
	priv->dev_max_rx_buffer_size = GVE_RX_BUFFER_SIZE_DQO;
	priv->header_buf_size = 0;

	if (dev_op_buffer_sizes &&
	    (supported_features_mask & GVE_SUP_BUFFER_SIZES_MASK)) {
		dev_info(&priv->pdev->dev,
			 "BUFFER SIZES device option enabled.\n");
		buf_size = be16_to_cpu(dev_op_buffer_sizes->packet_buffer_size);
		if (buf_size) {
			priv->dev_max_rx_buffer_size = buf_size;
			if (priv->dev_max_rx_buffer_size &
				(priv->dev_max_rx_buffer_size - 1))
				priv->dev_max_rx_buffer_size = GVE_RX_BUFFER_SIZE_DQO;
			if (priv->dev_max_rx_buffer_size < GVE_MIN_RX_BUFFER_SIZE)
				priv->dev_max_rx_buffer_size = GVE_MIN_RX_BUFFER_SIZE;
			if (priv->dev_max_rx_buffer_size > GVE_MAX_RX_BUFFER_SIZE)
				priv->dev_max_rx_buffer_size = GVE_MAX_RX_BUFFER_SIZE;
		}
		buf_size = be16_to_cpu(dev_op_buffer_sizes->header_buffer_size);
		if (buf_size) {
			priv->header_buf_size = buf_size;
			if (priv->header_buf_size & (priv->header_buf_size - 1))
				priv->header_buf_size =
					GVE_HEADER_BUFFER_SIZE_DEFAULT;
			if (priv->header_buf_size < GVE_HEADER_BUFFER_SIZE_MIN)
				priv->header_buf_size = GVE_HEADER_BUFFER_SIZE_MIN;
			if (priv->header_buf_size > GVE_HEADER_BUFFER_SIZE_MAX)
				priv->header_buf_size = GVE_HEADER_BUFFER_SIZE_MAX;
		}
	}

	if (dev_op_flow_steering &&
	    (supported_features_mask & GVE_SUP_FLOW_STEERING_MASK)) {
		dev_info(&priv->pdev->dev,
			 "FLOW STEERING device option enabled.\n");
		priv->flow_rules_max =
			be16_to_cpu(dev_op_flow_steering->max_num_rules);
	}

	/* Override pages for qpl for DQO-QPL */
	if (dev_op_dqo_qpl) {
		priv->tx_pages_per_qpl =
			be16_to_cpu(dev_op_dqo_qpl->tx_pages_per_qpl);
		priv->rx_pages_per_qpl =
			be16_to_cpu(dev_op_dqo_qpl->rx_pages_per_qpl);
		if (priv->tx_pages_per_qpl == 0)
			priv->tx_pages_per_qpl = DQO_QPL_DEFAULT_TX_PAGES;
		if (priv->rx_pages_per_qpl == 0)
			priv->rx_pages_per_qpl = DQO_QPL_DEFAULT_RX_PAGES;
	}
}

int gve_adminq_describe_device(struct gve_priv *priv)
{
	struct gve_device_option_flow_steering *dev_op_flow_steering = NULL;
	struct gve_device_option_buffer_sizes *dev_op_buffer_sizes = NULL;
	struct gve_device_option_jumbo_frames *dev_op_jumbo_frames = NULL;
	struct gve_device_option_gqi_rda *dev_op_gqi_rda = NULL;
	struct gve_device_option_gqi_qpl *dev_op_gqi_qpl = NULL;
	struct gve_device_option_dqo_rda *dev_op_dqo_rda = NULL;
	struct gve_device_option_dqo_qpl *dev_op_dqo_qpl = NULL;
	struct gve_device_descriptor *descriptor;
	u32 supported_features_mask = 0;
	union gve_adminq_command cmd;
	dma_addr_t descriptor_bus;
	int err = 0;
	u8 *mac;
	u16 mtu;

	memset(&cmd, 0, sizeof(cmd));
	descriptor = dma_alloc_coherent(&priv->pdev->dev, PAGE_SIZE,
					&descriptor_bus, GFP_KERNEL);
	if (!descriptor)
		return -ENOMEM;
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_DESCRIBE_DEVICE);
	cmd.describe_device.device_descriptor_addr =
						cpu_to_be64(descriptor_bus);
	cmd.describe_device.device_descriptor_version =
			cpu_to_be32(GVE_ADMINQ_DEVICE_DESCRIPTOR_VERSION);
	cmd.describe_device.available_length = cpu_to_be32(PAGE_SIZE);

	err = gve_adminq_execute_cmd(priv, &cmd);
	if (err)
		goto free_device_descriptor;

	err = gve_process_device_options(priv, descriptor, &dev_op_gqi_rda,
					 &dev_op_gqi_qpl, &dev_op_dqo_rda,
					 &dev_op_jumbo_frames,
					 &dev_op_buffer_sizes,
					 &dev_op_flow_steering,
					 &dev_op_dqo_qpl);
	if (err)
		goto free_device_descriptor;

	/* If the GQI_RAW_ADDRESSING option is not enabled and the queue format
	 * is not set to GqiRda, choose the queue format in a priority order:
	 * DqoRda, DqoQpl, GqiRda, GqiQpl. Use GqiQpl as default.
	 */
	if (dev_op_dqo_rda) {
		priv->queue_format = GVE_DQO_RDA_FORMAT;
		dev_info(&priv->pdev->dev,
			 "Driver is running with DQO RDA queue format.\n");
		supported_features_mask =
			be32_to_cpu(dev_op_dqo_rda->supported_features_mask);
	} else if (dev_op_dqo_qpl) {
		priv->queue_format = GVE_DQO_QPL_FORMAT;
		supported_features_mask =
			be32_to_cpu(dev_op_dqo_qpl->supported_features_mask);
	} else if (dev_op_gqi_rda) {
		priv->queue_format = GVE_GQI_RDA_FORMAT;
		dev_info(&priv->pdev->dev,
			 "Driver is running with GQI RDA queue format.\n");
		supported_features_mask =
			be32_to_cpu(dev_op_gqi_rda->supported_features_mask);
	} else if (priv->queue_format == GVE_GQI_RDA_FORMAT) {
		dev_info(&priv->pdev->dev,
			 "Driver is running with GQI RDA queue format.\n");
	} else {
		priv->queue_format = GVE_GQI_QPL_FORMAT;
		if (dev_op_gqi_qpl)
			supported_features_mask =
				be32_to_cpu(dev_op_gqi_qpl->supported_features_mask);
		dev_info(&priv->pdev->dev,
			 "Driver is running with GQI QPL queue format.\n");
	}
	if (gve_is_gqi(priv)) {
		err = gve_set_desc_cnt(priv, descriptor);
	} else {
		/* DQO supports LRO and flow-steering */
		priv->dev->hw_features |= NETIF_F_LRO;
		priv->dev->hw_features |= NETIF_F_NTUPLE;
		err = gve_set_desc_cnt_dqo(priv, descriptor, dev_op_dqo_rda);
	}
	if (err)
		goto free_device_descriptor;

	priv->max_registered_pages =
				be64_to_cpu(descriptor->max_registered_pages);
	mtu = be16_to_cpu(descriptor->mtu);
	if (mtu < ETH_MIN_MTU) {
		dev_err(&priv->pdev->dev, "MTU %d below minimum MTU\n", mtu);
		err = -EINVAL;
		goto free_device_descriptor;
	}
	priv->dev->max_mtu = mtu;
	priv->num_event_counters = be16_to_cpu(descriptor->counters);
	eth_hw_addr_set(priv->dev, descriptor->mac);
	mac = descriptor->mac;
	dev_info(&priv->pdev->dev, "MAC addr: %pM\n", mac);
	priv->tx_pages_per_qpl = be16_to_cpu(descriptor->tx_pages_per_qpl);
	priv->rx_data_slot_cnt = be16_to_cpu(descriptor->rx_pages_per_qpl);

	if (gve_is_gqi(priv) && priv->rx_data_slot_cnt < priv->rx_desc_cnt) {
		dev_err(&priv->pdev->dev, "rx_data_slot_cnt cannot be smaller than rx_desc_cnt, setting rx_desc_cnt down to %d.\n",
			priv->rx_data_slot_cnt);
		priv->rx_desc_cnt = priv->rx_data_slot_cnt;
	}
	priv->default_num_queues = be16_to_cpu(descriptor->default_num_queues);

	gve_enable_supported_features(priv, supported_features_mask,
				      dev_op_jumbo_frames,
				      dev_op_buffer_sizes,
				      dev_op_flow_steering,
				      dev_op_dqo_qpl);

free_device_descriptor:
	dma_free_coherent(&priv->pdev->dev, PAGE_SIZE, descriptor,
			  descriptor_bus);
	return err;
}

int gve_adminq_register_page_list(struct gve_priv *priv,
				  struct gve_queue_page_list *qpl)
{
	struct device *hdev = &priv->pdev->dev;
	u32 num_entries = qpl->num_entries;
	u32 size = num_entries * sizeof(qpl->page_buses[0]);
	union gve_adminq_command cmd;
	dma_addr_t page_list_bus;
	__be64 *page_list;
	int err;
	int i;

	memset(&cmd, 0, sizeof(cmd));
	page_list = dma_alloc_coherent(hdev, size, &page_list_bus, GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	for (i = 0; i < num_entries; i++)
		page_list[i] = cpu_to_be64(qpl->page_buses[i]);

	cmd.opcode = cpu_to_be32(GVE_ADMINQ_REGISTER_PAGE_LIST);
	cmd.reg_page_list = (struct gve_adminq_register_page_list) {
		.page_list_id = cpu_to_be32(qpl->id),
		.num_pages = cpu_to_be32(num_entries),
		.page_address_list_addr = cpu_to_be64(page_list_bus),
	};

	err = gve_adminq_execute_cmd(priv, &cmd);
	dma_free_coherent(hdev, size, page_list, page_list_bus);
	return err;
}

int gve_adminq_unregister_page_list(struct gve_priv *priv, u32 page_list_id)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_UNREGISTER_PAGE_LIST);
	cmd.unreg_page_list = (struct gve_adminq_unregister_page_list) {
		.page_list_id = cpu_to_be32(page_list_id),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_set_mtu(struct gve_priv *priv, u64 mtu)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_SET_DRIVER_PARAMETER);
	cmd.set_driver_param = (struct gve_adminq_set_driver_parameter) {
		.parameter_type = cpu_to_be32(GVE_SET_PARAM_MTU),
		.parameter_value = cpu_to_be64(mtu),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_report_stats(struct gve_priv *priv, u64 stats_report_len,
			    dma_addr_t stats_report_addr, u64 interval)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_REPORT_STATS);
	cmd.report_stats = (struct gve_adminq_report_stats) {
		.stats_report_len = cpu_to_be64(stats_report_len),
		.stats_report_addr = cpu_to_be64(stats_report_addr),
		.interval = cpu_to_be64(interval),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_verify_driver_compatibility(struct gve_priv *priv,
					   u64 driver_info_len,
					   dma_addr_t driver_info_addr)
{
	union gve_adminq_command cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_VERIFY_DRIVER_COMPATIBILITY);
	cmd.verify_driver_compatibility = (struct gve_adminq_verify_driver_compatibility) {
		.driver_info_len = cpu_to_be64(driver_info_len),
		.driver_info_addr = cpu_to_be64(driver_info_addr),
	};

	return gve_adminq_execute_cmd(priv, &cmd);
}

int gve_adminq_report_link_speed(struct gve_priv *priv)
{
	union gve_adminq_command gvnic_cmd;
	dma_addr_t link_speed_region_bus;
	__be64 *link_speed_region;
	int err;

	link_speed_region =
		dma_alloc_coherent(&priv->pdev->dev, sizeof(*link_speed_region),
				   &link_speed_region_bus, GFP_KERNEL);

	if (!link_speed_region)
		return -ENOMEM;

	memset(&gvnic_cmd, 0, sizeof(gvnic_cmd));
	gvnic_cmd.opcode = cpu_to_be32(GVE_ADMINQ_REPORT_LINK_SPEED);
	gvnic_cmd.report_link_speed.link_speed_address =
		cpu_to_be64(link_speed_region_bus);

	err = gve_adminq_execute_cmd(priv, &gvnic_cmd);

	priv->link_speed = be64_to_cpu(*link_speed_region);
	dma_free_coherent(&priv->pdev->dev, sizeof(*link_speed_region), link_speed_region,
			  link_speed_region_bus);
	return err;
}

int gve_adminq_get_ptype_map_dqo(struct gve_priv *priv,
				 struct gve_ptype_lut *ptype_lut)
{
	struct gve_ptype_map *ptype_map;
	union gve_adminq_command cmd;
	dma_addr_t ptype_map_bus;
	int err = 0;
	int i;

	memset(&cmd, 0, sizeof(cmd));
	ptype_map = dma_alloc_coherent(&priv->pdev->dev, sizeof(*ptype_map),
				       &ptype_map_bus, GFP_KERNEL);
	if (!ptype_map)
		return -ENOMEM;

	cmd.opcode = cpu_to_be32(GVE_ADMINQ_GET_PTYPE_MAP);
	cmd.get_ptype_map = (struct gve_adminq_get_ptype_map) {
		.ptype_map_len = cpu_to_be64(sizeof(*ptype_map)),
		.ptype_map_addr = cpu_to_be64(ptype_map_bus),
	};

	err = gve_adminq_execute_cmd(priv, &cmd);
	if (err)
		goto err;

	/* Populate ptype_lut. */
	for (i = 0; i < GVE_NUM_PTYPES; i++) {
		ptype_lut->ptypes[i].l3_type =
			ptype_map->ptypes[i].l3_type;
		ptype_lut->ptypes[i].l4_type =
			ptype_map->ptypes[i].l4_type;
	}
err:
	dma_free_coherent(&priv->pdev->dev, sizeof(*ptype_map), ptype_map,
			  ptype_map_bus);
	return err;
}

static int gve_adminq_configure_flow_rule(struct gve_priv *priv,
		struct gve_adminq_configure_flow_rule *flow_rule_cmd)
{
	return gve_adminq_execute_extended_cmd(priv,
			GVE_ADMINQ_CONFIGURE_FLOW_RULE,
			sizeof(struct gve_adminq_configure_flow_rule),
			flow_rule_cmd);
}

int gve_adminq_add_flow_rule(struct gve_priv *priv,
			     struct gve_flow_rule *rule)
{
	struct gve_adminq_configure_flow_rule flow_rule_cmd = {
		.cmd = cpu_to_be16(GVE_RULE_ADD),
		.loc = cpu_to_be16(rule->loc),
		.rule = {
			.flow_type = cpu_to_be16(rule->flow_type),
			.action = cpu_to_be16(rule->action),
			.key = {
				.src_ip = { rule->key.src_ip[0],
					    rule->key.src_ip[1],
					    rule->key.src_ip[2],
					    rule->key.src_ip[3] },
				.dst_ip = { rule->key.dst_ip[0],
					    rule->key.dst_ip[1],
					    rule->key.dst_ip[2],
					    rule->key.dst_ip[3] },
			},
			.mask = {
				.src_ip = { rule->mask.src_ip[0],
					    rule->mask.src_ip[1],
					    rule->mask.src_ip[2],
					    rule->mask.src_ip[3] },
				.dst_ip = { rule->mask.dst_ip[0],
					    rule->mask.dst_ip[1],
					    rule->mask.dst_ip[2],
					    rule->mask.dst_ip[3] },
			},
		},
	};
	switch (rule->flow_type) {
	case GVE_FLOW_TYPE_TCPV4:
	case GVE_FLOW_TYPE_UDPV4:
	case GVE_FLOW_TYPE_SCTPV4:
		flow_rule_cmd.rule.key.src_port = rule->key.src_port;
		flow_rule_cmd.rule.key.dst_port = rule->key.dst_port;
		flow_rule_cmd.rule.key.tos = rule->key.tos;
		flow_rule_cmd.rule.mask.src_port = rule->mask.src_port;
		flow_rule_cmd.rule.mask.dst_port = rule->mask.dst_port;
		flow_rule_cmd.rule.mask.tos = rule->mask.tos;
		break;
	case GVE_FLOW_TYPE_AHV4:
	case GVE_FLOW_TYPE_ESPV4:
		flow_rule_cmd.rule.key.spi = rule->key.spi;
		flow_rule_cmd.rule.key.tos = rule->key.tos;
		flow_rule_cmd.rule.mask.spi = rule->mask.spi;
		flow_rule_cmd.rule.mask.tos = rule->mask.tos;
		break;
	case GVE_FLOW_TYPE_TCPV6:
	case GVE_FLOW_TYPE_UDPV6:
	case GVE_FLOW_TYPE_SCTPV6:
		flow_rule_cmd.rule.key.src_port = rule->key.src_port;
		flow_rule_cmd.rule.key.dst_port = rule->key.dst_port;
		flow_rule_cmd.rule.key.tclass = rule->key.tclass;
		flow_rule_cmd.rule.mask.src_port = rule->mask.src_port;
		flow_rule_cmd.rule.mask.dst_port = rule->mask.dst_port;
		flow_rule_cmd.rule.mask.tclass = rule->mask.tclass;
		break;
	case GVE_FLOW_TYPE_AHV6:
	case GVE_FLOW_TYPE_ESPV6:
		flow_rule_cmd.rule.key.spi = rule->key.spi;
		flow_rule_cmd.rule.key.tclass = rule->key.tclass;
		flow_rule_cmd.rule.mask.spi = rule->mask.spi;
		flow_rule_cmd.rule.mask.tclass = rule->mask.tclass;
		break;
	}

	return gve_adminq_configure_flow_rule(priv, &flow_rule_cmd);
}

int gve_adminq_del_flow_rule(struct gve_priv *priv, int loc)
{
	struct gve_adminq_configure_flow_rule flow_rule_cmd = {
		.cmd = cpu_to_be16(GVE_RULE_DEL),
		.loc = cpu_to_be16(loc),
	};
	return gve_adminq_configure_flow_rule(priv, &flow_rule_cmd);
}

int gve_adminq_reset_flow_rules(struct gve_priv *priv)
{
	struct gve_adminq_configure_flow_rule flow_rule_cmd = {
		.cmd = cpu_to_be16(GVE_RULE_RESET),
	};
	return gve_adminq_configure_flow_rule(priv, &flow_rule_cmd);
}

int gve_adminq_configure_rss(struct gve_priv *priv,
			     struct gve_rss_config  *rss_config)
{
	dma_addr_t indir_bus = 0, key_bus = 0;
	union gve_adminq_command cmd;
	__be32 *indir = NULL;
	u8 *key = NULL;
	int err = 0;
	int i;

	if (rss_config->indir_size) {
		indir = dma_alloc_coherent(&priv->pdev->dev,
					   rss_config->indir_size *
						   sizeof(*rss_config->indir),
					   &indir_bus, GFP_KERNEL);
		if (!indir) {
			err = -ENOMEM;
			goto out;
		}
		for (i = 0; i < rss_config->indir_size; i++)
			indir[i] = cpu_to_be32(rss_config->indir[i]);
	}

	if (rss_config->key_size) {
		key = dma_alloc_coherent(&priv->pdev->dev,
					 rss_config->key_size *
						 sizeof(*rss_config->key),
					 &key_bus, GFP_KERNEL);
		if (!key) {
			err = -ENOMEM;
			goto out;
		}
		memcpy(key, rss_config->key, rss_config->key_size);
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = cpu_to_be32(GVE_ADMINQ_CONFIGURE_RSS);
	cmd.configure_rss = (struct gve_adminq_configure_rss) {
		.hash_types = cpu_to_be16(GVE_RSS_HASH_TCPV4 |
					  GVE_RSS_HASH_UDPV4 |
					  GVE_RSS_HASH_TCPV6 |
					  GVE_RSS_HASH_UDPV6),
		.halg = rss_config->alg,
		.hkey_len = cpu_to_be16(rss_config->key_size),
		.indir_len = cpu_to_be16(rss_config->indir_size),
		.hkey_addr = cpu_to_be64(key_bus),
		.indir_addr = cpu_to_be64(indir_bus),
	};

	err = gve_adminq_execute_cmd(priv, &cmd);

out:
	if (indir)
		dma_free_coherent(&priv->pdev->dev,
				  rss_config->indir_size *
					  sizeof(*rss_config->indir),
				  indir, indir_bus);
	if (key)
		dma_free_coherent(&priv->pdev->dev,
				  rss_config->key_size *
					  sizeof(*rss_config->key),
				  key, key_bus);
	return err;
}

