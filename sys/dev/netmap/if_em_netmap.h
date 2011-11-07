/*
 * (C) 2011 Luigi Rizzo, Matteo Landi - Universita` di Pisa
 *
 * BSD Copyright
 *
 * $Id$
 *
 * netmap changes for if_em.
 */

#include <net/netmap.h>
#include <sys/selinfo.h>
#include <vm/vm.h>
#include <vm/pmap.h>    /* vtophys ? */
#include <dev/netmap/netmap_kern.h>

static void	em_netmap_block_tasks(struct adapter *);
static void	em_netmap_unblock_tasks(struct adapter *);
static int	em_netmap_reg(struct ifnet *, int onoff);
static int	em_netmap_txsync(void *, u_int, int);
static int	em_netmap_rxsync(void *, u_int, int);
static void	em_netmap_lock_wrapper(void *, int, u_int);

static void
em_netmap_attach(struct adapter *adapter)
{
	struct netmap_adapter na;

	bzero(&na, sizeof(na));

	na.ifp = adapter->ifp;
	na.separate_locks = 1;
	na.num_tx_desc = adapter->num_tx_desc;
	na.num_rx_desc = adapter->num_rx_desc;
	na.nm_txsync = em_netmap_txsync;
	na.nm_rxsync = em_netmap_rxsync;
	na.nm_lock = em_netmap_lock_wrapper;
	na.nm_register = em_netmap_reg;
	/*
	 * adapter->rx_mbuf_sz is set by SIOCSETMTU, but in netmap mode
	 * we allocate the buffers on the first register. So we must
	 * disallow a SIOCSETMTU when if_capenable & IFCAP_NETMAP is set.
	 */
	na.buff_size = MCLBYTES;
	netmap_attach(&na, adapter->num_queues);
}


/*
 * wrapper to export locks to the generic code
 */
static void
em_netmap_lock_wrapper(void *_a, int what, u_int queueid)
{
	struct adapter *adapter = _a;

	ASSERT(queueid < adapter->num_queues);
	switch (what) {
		case NETMAP_CORE_LOCK:
		EM_CORE_LOCK(adapter);
		break;
	case NETMAP_CORE_UNLOCK:
		EM_CORE_UNLOCK(adapter);
		break;
	case NETMAP_TX_LOCK:
		EM_TX_LOCK(&adapter->tx_rings[queueid]);
		break;
	case NETMAP_TX_UNLOCK:
		EM_TX_UNLOCK(&adapter->tx_rings[queueid]);
		break;
	case NETMAP_RX_LOCK:
		EM_RX_LOCK(&adapter->rx_rings[queueid]);
		break;
	case NETMAP_RX_UNLOCK:
		EM_RX_UNLOCK(&adapter->rx_rings[queueid]);
		break;
	}
}


static void
em_netmap_block_tasks(struct adapter *adapter)
{
	if (adapter->msix > 1) { /* MSIX */
		int i;
		struct tx_ring *txr = adapter->tx_rings;
		struct rx_ring *rxr = adapter->rx_rings;

		for (i = 0; i < adapter->num_queues; i++, txr++, rxr++) {
			taskqueue_block(txr->tq);
			taskqueue_drain(txr->tq, &txr->tx_task);
			taskqueue_block(rxr->tq);
			taskqueue_drain(rxr->tq, &rxr->rx_task);
		}
	} else {	/* legacy */
		taskqueue_block(adapter->tq);
		taskqueue_drain(adapter->tq, &adapter->link_task);
		taskqueue_drain(adapter->tq, &adapter->que_task);
	}
}


static void
em_netmap_unblock_tasks(struct adapter *adapter)
{
	if (adapter->msix > 1) {
		struct tx_ring *txr = adapter->tx_rings;
		struct rx_ring *rxr = adapter->rx_rings;
		int i;

		for (i = 0; i < adapter->num_queues; i++) {
			taskqueue_unblock(txr->tq);
			taskqueue_unblock(rxr->tq);
		}
	} else { /* legacy */
		taskqueue_unblock(adapter->tq);
	}
}

/*
 * register-unregister routine
 */
static int
em_netmap_reg(struct ifnet *ifp, int onoff)
{
	struct adapter *adapter = ifp->if_softc;
	struct netmap_adapter *na = NA(ifp);
	int error = 0;

	if (na == NULL)
		return EINVAL;	/* no netmap support here */

	em_disable_intr(adapter);

	/* Tell the stack that the interface is no longer active */
	ifp->if_drv_flags &= ~(IFF_DRV_RUNNING | IFF_DRV_OACTIVE);

	em_netmap_block_tasks(adapter);

	if (onoff) {
		ifp->if_capenable |= IFCAP_NETMAP;

		/* save if_transmit for later restore.
		 * XXX also if_start and if_qflush ?
		 */
		na->if_transmit = ifp->if_transmit;
		ifp->if_transmit = netmap_start;

		em_init_locked(adapter);
		if ((ifp->if_drv_flags & (IFF_DRV_RUNNING | IFF_DRV_OACTIVE)) == 0) {
			error = ENOMEM;
			goto fail;
		}
	} else {
fail:
		/* restore if_transmit */
		ifp->if_transmit = na->if_transmit;
		ifp->if_capenable &= ~IFCAP_NETMAP;
		em_init_locked(adapter);	/* also enable intr */

	}
	em_netmap_unblock_tasks(adapter);
	return (error);
}

/*
 * Reconcile hardware and user view of the transmit ring, see
 * ixgbe.c for details.
 */
static int
em_netmap_txsync(void *a, u_int ring_nr, int do_lock)
{
	struct adapter *adapter = a;
	struct tx_ring *txr = &adapter->tx_rings[ring_nr];
	struct netmap_adapter *na = NA(adapter->ifp);
	struct netmap_kring *kring = &na->tx_rings[ring_nr];
	struct netmap_ring *ring = kring->ring;
	int j, k, n, lim = kring->nkr_num_slots - 1;

	/* generate an interrupt approximately every half ring */
	int report_frequency = kring->nkr_num_slots >> 1;

	k = ring->cur;
	if ( (kring->nr_kflags & NR_REINIT) || k > lim)
		return netmap_ring_reinit(kring);

	if (do_lock)
		EM_TX_LOCK(txr);
	bus_dmamap_sync(txr->txdma.dma_tag, txr->txdma.dma_map,
			BUS_DMASYNC_POSTREAD);

	/* record completed transmissions TODO
	 *
	 * instead of using TDH, we could read the transmitted status bit.
	 */
	j = E1000_READ_REG(&adapter->hw, E1000_TDH(ring_nr));
	if (j >= kring->nkr_num_slots) { /* XXX can happen */
		D("TDH wrap %d", j);
		j -= kring->nkr_num_slots;
	}
	int delta = j - txr->next_to_clean;
	if (delta) {
		/* new transmissions were completed, increment
		   ring->nr_hwavail. */
		if (delta < 0)
			delta += kring->nkr_num_slots;
		txr->next_to_clean = j;
		kring->nr_hwavail += delta;
	}

	/* update avail to what the hardware knows */
	ring->avail = kring->nr_hwavail;

	j = kring->nr_hwcur;
	if (j != k) {	/* we have packets to send */
		n = 0;
		while (j != k) {
			struct netmap_slot *slot = &ring->slot[j];
			struct e1000_tx_desc *curr = &txr->tx_base[j];
			struct em_buffer *txbuf = &txr->tx_buffers[j];
			int flags = ((slot->flags & NS_REPORT) ||
				j == 0 || j == report_frequency) ?
					E1000_TXD_CMD_RS : 0;
			void *addr = NMB(slot);
			int len = slot->len;
			if (addr == netmap_buffer_base || len > NETMAP_BUF_SIZE) {
				if (do_lock)
					EM_TX_UNLOCK(txr);
				return netmap_ring_reinit(kring);
			}

			slot->flags &= ~NS_REPORT;
			curr->upper.data = 0;
			curr->lower.data = 
			    htole32(
				adapter->txd_cmd |
				(E1000_TXD_CMD_EOP | flags) |
				slot->len);
			if (slot->flags & NS_BUF_CHANGED) {
				curr->buffer_addr = htole64(vtophys(addr));
				/* buffer has changed, unload and reload map */
				netmap_reload_map(txr->txtag, txbuf->map,
					addr, na->buff_size);
				slot->flags &= ~NS_BUF_CHANGED;
			}

			bus_dmamap_sync(txr->txtag, txbuf->map,
				BUS_DMASYNC_PREWRITE);
			j = (j == lim) ? 0 : j + 1;
			n++;
		}
		kring->nr_hwcur = ring->cur;

		/* decrease avail by number of sent packets */
		ring->avail -= n;
		kring->nr_hwavail = ring->avail;

		bus_dmamap_sync(txr->txdma.dma_tag, txr->txdma.dma_map,
			BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

		E1000_WRITE_REG(&adapter->hw, E1000_TDT(txr->me),
			ring->cur);
	}
	if (do_lock)
		EM_TX_UNLOCK(txr);
	return 0;
}

/*
 * Reconcile kernel and user view of the receive ring, see ixgbe.c
 */
static int
em_netmap_rxsync(void *a, u_int ring_nr, int do_lock)
{
	struct adapter *adapter = a;
	struct rx_ring *rxr = &adapter->rx_rings[ring_nr];
	struct netmap_adapter *na = NA(adapter->ifp);
	struct netmap_kring *kring = &na->rx_rings[ring_nr];
	struct netmap_ring *ring = kring->ring;
	int j, k, n, lim = kring->nkr_num_slots - 1;

	k = ring->cur;
	if ( (kring->nr_kflags & NR_REINIT) || k > lim)
		return netmap_ring_reinit(kring);
 
	if (do_lock)
		EM_RX_LOCK(rxr);
	/* XXX check sync modes */
	bus_dmamap_sync(rxr->rxdma.dma_tag, rxr->rxdma.dma_map,
			BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	/* acknowledge all the received packets. */
	j = rxr->next_to_check;
	for (n = 0; ; n++) {
		struct e1000_rx_desc *curr = &rxr->rx_base[j];

		if ((curr->status & E1000_RXD_STAT_DD) == 0)
			break;
		ring->slot[j].len = le16toh(curr->length);
		bus_dmamap_sync(rxr->tag, rxr->rx_buffers[j].map,
			BUS_DMASYNC_POSTREAD);
		j = (j == lim) ? 0 : j + 1;
	}
	if (n) {
		rxr->next_to_check = j;
		kring->nr_hwavail += n;
	}

	/* skip past packets that userspace has already processed:
	 * making them available for reception.
	 * advance nr_hwcur and issue a bus_dmamap_sync on the
	 * buffers so it is safe to write to them.
	 * Also increase nr_hwavail
         */
	j = kring->nr_hwcur;
	if (j != k) { /* userspace has read some packets. */
		n = 0;
		while (j != k) {
			struct netmap_slot *slot = &ring->slot[j];
			struct e1000_rx_desc *curr = &rxr->rx_base[j];
			struct em_buffer *rxbuf = &rxr->rx_buffers[j];
			void *addr = NMB(slot);

			if (addr == netmap_buffer_base) { /* bad buf */
				if (do_lock)
					EM_RX_UNLOCK(rxr);
				return netmap_ring_reinit(kring);
			}

			curr->status = 0;
			if (slot->flags & NS_BUF_CHANGED) {
				curr->buffer_addr = htole64(vtophys(addr));
				/* buffer has changed, unload and reload map */
				netmap_reload_map(rxr->rxtag, rxbuf->map,
					addr, na->buff_size);
				slot->flags &= ~NS_BUF_CHANGED;
			}

			bus_dmamap_sync(rxr->rxtag, rxbuf->map,
				BUS_DMASYNC_PREREAD);

			j = (j == lim) ? 0 : j + 1;
			n++;
		}
		kring->nr_hwavail -= n;
		kring->nr_hwcur = ring->cur;
		bus_dmamap_sync(rxr->rxdma.dma_tag, rxr->rxdma.dma_map,
			BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
		/*
		 * IMPORTANT: we must leave one free slot in the ring,
		 * so move j back by one unit
		 */
		j = (j == 0) ? lim : j - 1;
		E1000_WRITE_REG(&adapter->hw, E1000_RDT(rxr->me), j);
	}
	/* tell userspace that there are new packets */
	ring->avail = kring->nr_hwavail ;
	if (do_lock)
		EM_RX_UNLOCK(rxr);
	return 0;
}