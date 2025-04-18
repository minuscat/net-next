// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2018 Intel Corporation. */

#include <linux/bpf_trace.h>
#include <net/xdp_sock_drv.h>
#include <net/xdp.h>

#include "ixgbe.h"
#include "ixgbe_txrx_common.h"

struct xsk_buff_pool *ixgbe_xsk_pool(struct ixgbe_adapter *adapter,
				     struct ixgbe_ring *ring)
{
	bool xdp_on = READ_ONCE(adapter->xdp_prog);
	int qid = ring->ring_idx;

	if (!xdp_on || !test_bit(qid, adapter->af_xdp_zc_qps))
		return NULL;

	return xsk_get_pool_from_qid(adapter->netdev, qid);
}

static int ixgbe_xsk_pool_enable(struct ixgbe_adapter *adapter,
				 struct xsk_buff_pool *pool,
				 u16 qid)
{
	struct net_device *netdev = adapter->netdev;
	bool if_running;
	int err;

	if (qid >= adapter->num_rx_queues)
		return -EINVAL;

	if (qid >= netdev->real_num_rx_queues ||
	    qid >= netdev->real_num_tx_queues)
		return -EINVAL;

	err = xsk_pool_dma_map(pool, &adapter->pdev->dev, IXGBE_RX_DMA_ATTR);
	if (err)
		return err;

	if_running = netif_running(adapter->netdev) &&
		     ixgbe_enabled_xdp_adapter(adapter);

	if (if_running)
		ixgbe_txrx_ring_disable(adapter, qid);

	set_bit(qid, adapter->af_xdp_zc_qps);

	if (if_running) {
		ixgbe_txrx_ring_enable(adapter, qid);

		/* Kick start the NAPI context so that receiving will start */
		err = ixgbe_xsk_wakeup(adapter->netdev, qid, XDP_WAKEUP_RX);
		if (err) {
			clear_bit(qid, adapter->af_xdp_zc_qps);
			xsk_pool_dma_unmap(pool, IXGBE_RX_DMA_ATTR);
			return err;
		}
	}

	return 0;
}

static int ixgbe_xsk_pool_disable(struct ixgbe_adapter *adapter, u16 qid)
{
	struct xsk_buff_pool *pool;
	bool if_running;

	pool = xsk_get_pool_from_qid(adapter->netdev, qid);
	if (!pool)
		return -EINVAL;

	if_running = netif_running(adapter->netdev) &&
		     ixgbe_enabled_xdp_adapter(adapter);

	if (if_running)
		ixgbe_txrx_ring_disable(adapter, qid);

	clear_bit(qid, adapter->af_xdp_zc_qps);
	xsk_pool_dma_unmap(pool, IXGBE_RX_DMA_ATTR);

	if (if_running)
		ixgbe_txrx_ring_enable(adapter, qid);

	return 0;
}

int ixgbe_xsk_pool_setup(struct ixgbe_adapter *adapter,
			 struct xsk_buff_pool *pool,
			 u16 qid)
{
	return pool ? ixgbe_xsk_pool_enable(adapter, pool, qid) :
		ixgbe_xsk_pool_disable(adapter, qid);
}

static int ixgbe_run_xdp_zc(struct ixgbe_adapter *adapter,
			    struct ixgbe_ring *rx_ring,
			    struct xdp_buff *xdp)
{
	int err, result = IXGBE_XDP_PASS;
	struct bpf_prog *xdp_prog;
	struct ixgbe_ring *ring;
	struct xdp_frame *xdpf;
	u32 act;

	xdp_prog = READ_ONCE(rx_ring->xdp_prog);
	act = bpf_prog_run_xdp(xdp_prog, xdp);

	if (likely(act == XDP_REDIRECT)) {
		err = xdp_do_redirect(rx_ring->netdev, xdp, xdp_prog);
		if (!err)
			return IXGBE_XDP_REDIR;
		if (xsk_uses_need_wakeup(rx_ring->xsk_pool) && err == -ENOBUFS)
			result = IXGBE_XDP_EXIT;
		else
			result = IXGBE_XDP_CONSUMED;
		goto out_failure;
	}

	switch (act) {
	case XDP_PASS:
		break;
	case XDP_TX:
		xdpf = xdp_convert_buff_to_frame(xdp);
		if (unlikely(!xdpf))
			goto out_failure;
		ring = ixgbe_determine_xdp_ring(adapter);
		if (static_branch_unlikely(&ixgbe_xdp_locking_key))
			spin_lock(&ring->tx_lock);
		result = ixgbe_xmit_xdp_ring(ring, xdpf);
		if (static_branch_unlikely(&ixgbe_xdp_locking_key))
			spin_unlock(&ring->tx_lock);
		if (result == IXGBE_XDP_CONSUMED)
			goto out_failure;
		break;
	case XDP_DROP:
		result = IXGBE_XDP_CONSUMED;
		break;
	default:
		bpf_warn_invalid_xdp_action(rx_ring->netdev, xdp_prog, act);
		fallthrough;
	case XDP_ABORTED:
		result = IXGBE_XDP_CONSUMED;
out_failure:
		trace_xdp_exception(rx_ring->netdev, xdp_prog, act);
	}
	return result;
}

bool ixgbe_alloc_rx_buffers_zc(struct ixgbe_ring *rx_ring, u16 count)
{
	union ixgbe_adv_rx_desc *rx_desc;
	struct ixgbe_rx_buffer *bi;
	u16 i = rx_ring->next_to_use;
	dma_addr_t dma;
	bool ok = true;

	/* nothing to do */
	if (!count)
		return true;

	rx_desc = IXGBE_RX_DESC(rx_ring, i);
	bi = &rx_ring->rx_buffer_info[i];
	i -= rx_ring->count;

	do {
		bi->xdp = xsk_buff_alloc(rx_ring->xsk_pool);
		if (!bi->xdp) {
			ok = false;
			break;
		}

		dma = xsk_buff_xdp_get_dma(bi->xdp);

		/* Refresh the desc even if buffer_addrs didn't change
		 * because each write-back erases this info.
		 */
		rx_desc->read.pkt_addr = cpu_to_le64(dma);

		rx_desc++;
		bi++;
		i++;
		if (unlikely(!i)) {
			rx_desc = IXGBE_RX_DESC(rx_ring, 0);
			bi = rx_ring->rx_buffer_info;
			i -= rx_ring->count;
		}

		/* clear the length for the next_to_use descriptor */
		rx_desc->wb.upper.length = 0;

		count--;
	} while (count);

	i += rx_ring->count;

	if (rx_ring->next_to_use != i) {
		rx_ring->next_to_use = i;

		/* Force memory writes to complete before letting h/w
		 * know there are new descriptors to fetch.  (Only
		 * applicable for weak-ordered memory model archs,
		 * such as IA-64).
		 */
		wmb();
		writel(i, rx_ring->tail);
	}

	return ok;
}

static struct sk_buff *ixgbe_construct_skb_zc(struct ixgbe_ring *rx_ring,
					      const struct xdp_buff *xdp)
{
	unsigned int totalsize = xdp->data_end - xdp->data_meta;
	unsigned int metasize = xdp->data - xdp->data_meta;
	struct sk_buff *skb;

	net_prefetch(xdp->data_meta);

	/* allocate a skb to store the frags */
	skb = napi_alloc_skb(&rx_ring->q_vector->napi, totalsize);
	if (unlikely(!skb))
		return NULL;

	memcpy(__skb_put(skb, totalsize), xdp->data_meta,
	       ALIGN(totalsize, sizeof(long)));

	if (metasize) {
		skb_metadata_set(skb, metasize);
		__skb_pull(skb, metasize);
	}

	return skb;
}

static void ixgbe_inc_ntc(struct ixgbe_ring *rx_ring)
{
	u32 ntc = rx_ring->next_to_clean + 1;

	ntc = (ntc < rx_ring->count) ? ntc : 0;
	rx_ring->next_to_clean = ntc;
	prefetch(IXGBE_RX_DESC(rx_ring, ntc));
}

int ixgbe_clean_rx_irq_zc(struct ixgbe_q_vector *q_vector,
			  struct ixgbe_ring *rx_ring,
			  const int budget)
{
	unsigned int total_rx_bytes = 0, total_rx_packets = 0;
	struct ixgbe_adapter *adapter = q_vector->adapter;
	u16 cleaned_count = ixgbe_desc_unused(rx_ring);
	unsigned int xdp_res, xdp_xmit = 0;
	bool failure = false;
	struct sk_buff *skb;

	while (likely(total_rx_packets < budget)) {
		union ixgbe_adv_rx_desc *rx_desc;
		struct ixgbe_rx_buffer *bi;
		unsigned int size;

		/* return some buffers to hardware, one at a time is too slow */
		if (cleaned_count >= IXGBE_RX_BUFFER_WRITE) {
			failure = failure ||
				  !ixgbe_alloc_rx_buffers_zc(rx_ring,
							     cleaned_count);
			cleaned_count = 0;
		}

		rx_desc = IXGBE_RX_DESC(rx_ring, rx_ring->next_to_clean);
		size = le16_to_cpu(rx_desc->wb.upper.length);
		if (!size)
			break;

		/* This memory barrier is needed to keep us from reading
		 * any other fields out of the rx_desc until we know the
		 * descriptor has been written back
		 */
		dma_rmb();

		bi = &rx_ring->rx_buffer_info[rx_ring->next_to_clean];

		if (unlikely(!ixgbe_test_staterr(rx_desc,
						 IXGBE_RXD_STAT_EOP))) {
			struct ixgbe_rx_buffer *next_bi;

			xsk_buff_free(bi->xdp);
			bi->xdp = NULL;
			ixgbe_inc_ntc(rx_ring);
			next_bi =
			       &rx_ring->rx_buffer_info[rx_ring->next_to_clean];
			next_bi->discard = true;
			continue;
		}

		if (unlikely(bi->discard)) {
			xsk_buff_free(bi->xdp);
			bi->xdp = NULL;
			bi->discard = false;
			ixgbe_inc_ntc(rx_ring);
			continue;
		}

		bi->xdp->data_end = bi->xdp->data + size;
		xsk_buff_dma_sync_for_cpu(bi->xdp);
		xdp_res = ixgbe_run_xdp_zc(adapter, rx_ring, bi->xdp);

		if (likely(xdp_res & (IXGBE_XDP_TX | IXGBE_XDP_REDIR))) {
			xdp_xmit |= xdp_res;
		} else if (xdp_res == IXGBE_XDP_EXIT) {
			failure = true;
			break;
		} else if (xdp_res == IXGBE_XDP_CONSUMED) {
			xsk_buff_free(bi->xdp);
		} else if (xdp_res == IXGBE_XDP_PASS) {
			goto construct_skb;
		}

		bi->xdp = NULL;
		total_rx_packets++;
		total_rx_bytes += size;

		cleaned_count++;
		ixgbe_inc_ntc(rx_ring);
		continue;

construct_skb:
		/* XDP_PASS path */
		skb = ixgbe_construct_skb_zc(rx_ring, bi->xdp);
		if (!skb) {
			rx_ring->rx_stats.alloc_rx_buff_failed++;
			break;
		}

		xsk_buff_free(bi->xdp);
		bi->xdp = NULL;

		cleaned_count++;
		ixgbe_inc_ntc(rx_ring);

		if (eth_skb_pad(skb))
			continue;

		total_rx_bytes += skb->len;
		total_rx_packets++;

		ixgbe_process_skb_fields(rx_ring, rx_desc, skb);
		ixgbe_rx_skb(q_vector, skb);
	}

	if (xdp_xmit & IXGBE_XDP_REDIR)
		xdp_do_flush();

	if (xdp_xmit & IXGBE_XDP_TX) {
		struct ixgbe_ring *ring = ixgbe_determine_xdp_ring(adapter);

		ixgbe_xdp_ring_update_tail_locked(ring);
	}

	ixgbe_update_rx_ring_stats(rx_ring, q_vector, total_rx_packets,
				   total_rx_bytes);

	if (xsk_uses_need_wakeup(rx_ring->xsk_pool)) {
		if (failure || rx_ring->next_to_clean == rx_ring->next_to_use)
			xsk_set_rx_need_wakeup(rx_ring->xsk_pool);
		else
			xsk_clear_rx_need_wakeup(rx_ring->xsk_pool);

		return (int)total_rx_packets;
	}
	return failure ? budget : (int)total_rx_packets;
}

void ixgbe_xsk_clean_rx_ring(struct ixgbe_ring *rx_ring)
{
	struct ixgbe_rx_buffer *bi;
	u16 i;

	for (i = 0; i < rx_ring->count; i++) {
		bi = &rx_ring->rx_buffer_info[i];

		if (!bi->xdp)
			continue;

		xsk_buff_free(bi->xdp);
		bi->xdp = NULL;
	}
}

static bool ixgbe_xmit_zc(struct ixgbe_ring *xdp_ring, unsigned int budget)
{
	struct xsk_buff_pool *pool = xdp_ring->xsk_pool;
	union ixgbe_adv_tx_desc *tx_desc = NULL;
	struct ixgbe_tx_buffer *tx_bi;
	bool work_done = true;
	struct xdp_desc desc;
	dma_addr_t dma;
	u32 cmd_type;

	while (budget-- > 0) {
		if (unlikely(!ixgbe_desc_unused(xdp_ring))) {
			work_done = false;
			break;
		}

		if (!netif_carrier_ok(xdp_ring->netdev))
			break;

		if (!xsk_tx_peek_desc(pool, &desc))
			break;

		dma = xsk_buff_raw_get_dma(pool, desc.addr);
		xsk_buff_raw_dma_sync_for_device(pool, dma, desc.len);

		tx_bi = &xdp_ring->tx_buffer_info[xdp_ring->next_to_use];
		tx_bi->bytecount = desc.len;
		tx_bi->xdpf = NULL;
		tx_bi->gso_segs = 1;

		tx_desc = IXGBE_TX_DESC(xdp_ring, xdp_ring->next_to_use);
		tx_desc->read.buffer_addr = cpu_to_le64(dma);

		/* put descriptor type bits */
		cmd_type = IXGBE_ADVTXD_DTYP_DATA |
			   IXGBE_ADVTXD_DCMD_DEXT |
			   IXGBE_ADVTXD_DCMD_IFCS;
		cmd_type |= desc.len | IXGBE_TXD_CMD;
		tx_desc->read.cmd_type_len = cpu_to_le32(cmd_type);
		tx_desc->read.olinfo_status =
			cpu_to_le32(desc.len << IXGBE_ADVTXD_PAYLEN_SHIFT);

		xdp_ring->next_to_use++;
		if (xdp_ring->next_to_use == xdp_ring->count)
			xdp_ring->next_to_use = 0;
	}

	if (tx_desc) {
		ixgbe_xdp_ring_update_tail(xdp_ring);
		xsk_tx_release(pool);
	}

	return !!budget && work_done;
}

static void ixgbe_clean_xdp_tx_buffer(struct ixgbe_ring *tx_ring,
				      struct ixgbe_tx_buffer *tx_bi)
{
	xdp_return_frame(tx_bi->xdpf);
	dma_unmap_single(tx_ring->dev,
			 dma_unmap_addr(tx_bi, dma),
			 dma_unmap_len(tx_bi, len), DMA_TO_DEVICE);
	dma_unmap_len_set(tx_bi, len, 0);
}

bool ixgbe_clean_xdp_tx_irq(struct ixgbe_q_vector *q_vector,
			    struct ixgbe_ring *tx_ring, int napi_budget)
{
	u16 ntc = tx_ring->next_to_clean, ntu = tx_ring->next_to_use;
	unsigned int total_packets = 0, total_bytes = 0;
	struct xsk_buff_pool *pool = tx_ring->xsk_pool;
	union ixgbe_adv_tx_desc *tx_desc;
	struct ixgbe_tx_buffer *tx_bi;
	u32 xsk_frames = 0;

	tx_bi = &tx_ring->tx_buffer_info[ntc];
	tx_desc = IXGBE_TX_DESC(tx_ring, ntc);

	while (ntc != ntu) {
		if (!(tx_desc->wb.status & cpu_to_le32(IXGBE_TXD_STAT_DD)))
			break;

		total_bytes += tx_bi->bytecount;
		total_packets += tx_bi->gso_segs;

		if (tx_bi->xdpf)
			ixgbe_clean_xdp_tx_buffer(tx_ring, tx_bi);
		else
			xsk_frames++;

		tx_bi->xdpf = NULL;

		tx_bi++;
		tx_desc++;
		ntc++;
		if (unlikely(ntc == tx_ring->count)) {
			ntc = 0;
			tx_bi = tx_ring->tx_buffer_info;
			tx_desc = IXGBE_TX_DESC(tx_ring, 0);
		}

		/* issue prefetch for next Tx descriptor */
		prefetch(tx_desc);
	}

	tx_ring->next_to_clean = ntc;
	ixgbe_update_tx_ring_stats(tx_ring, q_vector, total_packets,
				   total_bytes);

	if (xsk_frames)
		xsk_tx_completed(pool, xsk_frames);

	if (xsk_uses_need_wakeup(pool))
		xsk_set_tx_need_wakeup(pool);

	return ixgbe_xmit_zc(tx_ring, q_vector->tx.work_limit);
}

int ixgbe_xsk_wakeup(struct net_device *dev, u32 qid, u32 flags)
{
	struct ixgbe_adapter *adapter = ixgbe_from_netdev(dev);
	struct ixgbe_ring *ring;

	if (test_bit(__IXGBE_DOWN, &adapter->state))
		return -ENETDOWN;

	if (!READ_ONCE(adapter->xdp_prog))
		return -EINVAL;

	if (qid >= adapter->num_xdp_queues)
		return -EINVAL;

	ring = adapter->xdp_ring[qid];

	if (test_bit(__IXGBE_TX_DISABLED, &ring->state))
		return -ENETDOWN;

	if (!ring->xsk_pool)
		return -EINVAL;

	if (!napi_if_scheduled_mark_missed(&ring->q_vector->napi)) {
		u64 eics = BIT_ULL(ring->q_vector->v_idx);

		ixgbe_irq_rearm_queues(adapter, eics);
	}

	return 0;
}

void ixgbe_xsk_clean_tx_ring(struct ixgbe_ring *tx_ring)
{
	u16 ntc = tx_ring->next_to_clean, ntu = tx_ring->next_to_use;
	struct xsk_buff_pool *pool = tx_ring->xsk_pool;
	struct ixgbe_tx_buffer *tx_bi;
	u32 xsk_frames = 0;

	while (ntc != ntu) {
		tx_bi = &tx_ring->tx_buffer_info[ntc];

		if (tx_bi->xdpf)
			ixgbe_clean_xdp_tx_buffer(tx_ring, tx_bi);
		else
			xsk_frames++;

		tx_bi->xdpf = NULL;

		ntc++;
		if (ntc == tx_ring->count)
			ntc = 0;
	}

	if (xsk_frames)
		xsk_tx_completed(pool, xsk_frames);
}
