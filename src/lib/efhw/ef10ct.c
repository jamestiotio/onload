/* SPDX-License-Identifier: GPL-2.0 */
/* X-SPDX-Copyright-Text: Copyright (C) 2023, Advanced Micro Devices, Inc. */

#include <ci/driver/kernel_compat.h>
#include <ci/driver/efab/hardware.h>
#include <ci/efhw/debug.h>
#include <ci/efhw/iopage.h>
#include <ci/efhw/nic.h>
#include <ci/efhw/eventq.h>
#include <ci/efhw/checks.h>
#include <ci/efhw/efhw_buftable.h>

#include <ci/driver/ci_ef10ct.h>
#include <ci/tools/bitfield.h>
#include <ci/tools/debug.h>

#include <ci/efhw/efct.h>
#include <ci/efhw/ef10ct.h>
#include <ci/efhw/mc_driver_pcol.h>

#include "etherfabric/internal/internal.h"

#include "ef10ct.h"


#if CI_HAVE_EF10CT


/*----------------------------------------------------------------------------
 *
 * MCDI helper
 *
 *---------------------------------------------------------------------------*/
int ef10ct_fw_rpc(struct efhw_nic *nic, struct efx_auxiliary_rpc *cmd)
{
  int rc;
  struct device *dev;
  struct efx_auxiliary_device* edev;
  struct efx_auxiliary_client* cli;

  /* FIXME need to handle reset stuff here */
  EFCT_PRE(dev, edev, cli, nic, rc);
  rc = edev->ops->fw_rpc(cli, cmd);
  EFCT_POST(dev, edev, cli, nic, rc);

  return rc;
}


/*----------------------------------------------------------------------------
 *
 * Initialisation and configuration discovery
 *
 *---------------------------------------------------------------------------*/


static void
ef10ct_nic_tweak_hardware(struct efhw_nic *nic)
{
}


static int
ef10ct_nic_init_hardware(struct efhw_nic *nic,
                         struct efhw_ev_handler *ev_handlers,
                         const uint8_t *mac_addr)
{
  memcpy(nic->mac_addr, mac_addr, ETH_ALEN);
  nic->ev_handlers = ev_handlers;
  nic->flags |= NIC_FLAG_TX_CTPIO | NIC_FLAG_CTPIO_ONLY
             | NIC_FLAG_HW_RX_TIMESTAMPING | NIC_FLAG_HW_TX_TIMESTAMPING
             | NIC_FLAG_RX_SHARED
             | NIC_FLAG_RX_FILTER_TYPE_IP_LOCAL
             | NIC_FLAG_RX_FILTER_TYPE_IP_FULL
             | NIC_FLAG_VLAN_FILTERS
             | NIC_FLAG_RX_FILTER_ETHERTYPE
             | NIC_FLAG_HW_MULTICAST_REPLICATION
             | NIC_FLAG_SHARED_PD
             /* TODO: This will need to be updated to check for nic capabilities. */
             | NIC_FLAG_RX_FILTER_TYPE_ETH_LOCAL
             | NIC_FLAG_RX_FILTER_TYPE_ETH_LOCAL_VLAN
             | NIC_FLAG_PHYS_CONTIG_EVQ
             | NIC_FLAG_EVQ_IRQ
             | NIC_FLAG_LLCT
             ;
  return 0;
}


static void
ef10ct_nic_release_hardware(struct efhw_nic *nic)
{
  EFHW_TRACE("%s:", __FUNCTION__);
}


/*--------------------------------------------------------------------
 *
 * Event Management - and SW event posting
 *
 *--------------------------------------------------------------------*/

static void ef10ct_check_for_flushes(struct work_struct *work)
{
  struct efhw_nic_ef10ct_evq *evq = 
    container_of(work, struct efhw_nic_ef10ct_evq, check_flushes.work);
  ci_qword_t *event = evq->base;
  bool found_flush = false;
  int txq;
  int i;

  /* In the case of a flush timeout this may have been rescheduled following
   * evq disable. In which case bail out now.
   */
  if( atomic_read(&evq->queues_flushing) < 0 )
    return;

  for(i = 0; i < evq->capacity; i++) {
    if(CI_QWORD_FIELD(*event, EFCT_EVENT_TYPE) == EFCT_EVENT_TYPE_CONTROL &&
       CI_QWORD_FIELD(*event, EFCT_CTRL_SUBTYPE) == EFCT_CTRL_EV_FLUSH &&
       CI_QWORD_FIELD(*event, EFCT_FLUSH_TYPE) == EFCT_FLUSH_TYPE_TX) {
      found_flush = true;
      txq = CI_QWORD_FIELD(*event, EFCT_FLUSH_QUEUE_ID);
      efhw_handle_txdmaq_flushed(evq->nic, txq);
      break;
    }
    event++;
  }

  if( !found_flush || !atomic_dec_and_test(&evq->queues_flushing) ) {
    EFHW_ERR("%s: WARNING: No TX flush found, scheduling delayed work",
             __FUNCTION__);
    schedule_delayed_work(&evq->check_flushes, 100);
  }
}


/* FIXME EF10CT
 * Need to handle timesync and credits
 * X3 net driver does dma mapping
 */
static int
ef10ct_nic_event_queue_enable(struct efhw_nic *nic, uint32_t client_id,
                              struct efhw_evq_params *efhw_params)
{
  struct efx_auxiliary_evq_params qparams = {
    .qid = efhw_params->evq,
    .entries = efhw_params->evq_size,
    /* We don't provide a pci_dev to enable queue memory to be mapped for us,
     * so we're given plain physical addresses.
     */
    .q_page = pfn_to_page(efhw_params->dma_addrs[0] >> PAGE_SHIFT),
    .page_offset = 0,
    .q_size = efhw_params->evq_size * sizeof(efhw_event_t),
    .subscribe_time_sync = efhw_params->flags & EFHW_VI_TX_TIMESTAMPS,
    .unsol_credit = efhw_params->flags & EFHW_VI_TX_TIMESTAMPS ? CI_CFG_TIME_SYNC_EVENT_EVQ_CAPACITY  - 1 : 0,
  };
  struct efhw_nic_ef10ct *ef10ct = nic->arch_extra;
  struct efhw_nic_ef10ct_evq *ef10ct_evq;
  int rc;
#ifndef NDEBUG
  int i;
#endif
  struct efx_auxiliary_rpc dummy;

  /* This is a dummy EVQ, so nothing to do. */
  if( efhw_params->evq >= ef10ct->evq_n )
    return 0;

  ef10ct_evq = &ef10ct->evq[efhw_params->evq];

  /* We only look at the first page because this memory should be physically
   * contiguous, but the API provides us with an address per 4K (NIC page)
   * chunk, so sanity check that there are enough pages for the size of queue
   * we're asking for.
   */
  EFHW_ASSERT(efhw_params->n_pages * EFHW_NIC_PAGES_IN_OS_PAGE * CI_PAGE_SIZE
	      >= qparams.q_size);
#ifndef NDEBUG
  /* We should have been provided with physical addresses of physically
   * contiguous memory, so sanity check the addresses look right.
   */
  for( i = 1; i < efhw_params->n_pages; i++ ) {
    EFHW_ASSERT(efhw_params->dma_addrs[i] - efhw_params->dma_addrs[i-1] ==
		EFHW_NIC_PAGE_SIZE);
  }
#endif

  dummy.cmd = MC_CMD_INIT_EVQ;
  dummy.inlen = sizeof(qparams);
  dummy.inbuf = (void*)&qparams;
  dummy.outlen = 0;
  rc = ef10ct_fw_rpc(nic, &dummy);

  if( rc == 0 ) {
    ef10ct_evq->nic = nic;
    ef10ct_evq->base = phys_to_virt(efhw_params->dma_addrs[0]);
    ef10ct_evq->capacity = efhw_params->evq_size;
    atomic_set(&ef10ct_evq->queues_flushing, 0);
    INIT_DELAYED_WORK(&ef10ct_evq->check_flushes, ef10ct_check_for_flushes);
  }

  return rc;
}

static void
ef10ct_nic_event_queue_disable(struct efhw_nic *nic, uint32_t client_id,
                               uint evq, int time_sync_events_enabled)
{
  struct efhw_nic_ef10ct *ef10ct = nic->arch_extra;
  struct efhw_nic_ef10ct_evq *ef10ct_evq;
  struct efx_auxiliary_rpc dummy;

  /* This is a dummy EVQ, so nothing to do. */
  if( evq >= ef10ct->evq_n )
    return;

  ef10ct_evq = &ef10ct->evq[evq];

  /* In the normal case we'll be disabling the queue because all outstanding
   * flushes have completed. However, in the case of a flush timeout there may
   * still be a work item scheduled. We want to avoid it rescheduling if so.
   */
  atomic_set(&ef10ct_evq->queues_flushing, -1);
  cancel_delayed_work_sync(&ef10ct_evq->check_flushes);

  dummy.cmd = MC_CMD_FINI_EVQ;
  dummy.inlen = sizeof(int);
  dummy.inbuf = &evq;
  dummy.outlen = 0;
  ef10ct_fw_rpc(nic, &dummy);
}

static void
ef10ct_nic_wakeup_request(struct efhw_nic *nic, volatile void __iomem* io_page,
                          int vi_id, int rptr)
{
}


static bool
ef10ct_accept_vi_constraints(struct efhw_nic *nic, int low, unsigned order,
                             void* arg)
{
  struct efhw_vi_constraints *vc = arg;
  struct efhw_nic_ef10ct *ef10ct = nic->arch_extra;

  EFHW_ERR("%s: FIXME SCJ want txq %d low %d evq_n %d txq %d", __func__,
           vc->want_txq, low, ef10ct->evq_n,
           low < ef10ct->evq_n ? ef10ct->evq[low].txq : -1);
  /* If this VI will want a TXQ it needs a HW EVQ. These all fall within
   * the range [0,ef10ct->evq_n). We use the space above that to provide
   * dummy EVQS. */
  if( vc->want_txq ) {
    if( low < ef10ct->evq_n )
      return ef10ct->evq[low].txq != EFCT_EVQ_NO_TXQ;
    else
      return false;
  }
  else {
    return low >= ef10ct->evq_n;
  }
  return true;
}


/*----------------------------------------------------------------------------
 *
 * DMAQ low-level register interface
 *
 *---------------------------------------------------------------------------*/


static int
ef10ct_dmaq_tx_q_init(struct efhw_nic *nic, uint32_t client_id,
                      struct efhw_dmaq_params *txq_params)
{
  struct efhw_nic_ef10ct *ef10ct = nic->arch_extra;
  struct efhw_nic_ef10ct_evq *ef10ct_evq = &ef10ct->evq[txq_params->evq];
  struct efx_auxiliary_txq_params params = {
    .evq = txq_params->evq,
    .qid = ef10ct_evq->txq,
    .label = txq_params->tag,
  };
  int rc;
  struct efx_auxiliary_rpc dummy;

  EFHW_ASSERT(txq_params->evq < ef10ct->evq_n);
  EFHW_ASSERT(params.qid != EFCT_EVQ_NO_TXQ);

  dummy.cmd = MC_CMD_INIT_TXQ;
  dummy.inlen = sizeof(struct efx_auxiliary_txq_params);
  dummy.inbuf = (void*)&params;
  dummy.outlen = sizeof(struct efx_auxiliary_txq_params);
  dummy.outbuf = (void*)&params;
  rc = ef10ct_fw_rpc(nic, &dummy);

  if( rc >= 0 ) {
    txq_params->tx.qid_out = rc;
    rc = 0;
  }

  return 0;
}


static int 
ef10ct_dmaq_rx_q_init(struct efhw_nic *nic, uint32_t client_id,
                      struct efhw_dmaq_params *params)
{
  return 0;
}

static size_t
ef10ct_max_shared_rxqs(struct efhw_nic *nic)
{
  /* FIXME SCJ efct vi requires this at the moment */
  return 8;
}


/*--------------------------------------------------------------------
 *
 * DMA Queues - mid level API
 *
 *--------------------------------------------------------------------*/


static int
ef10ct_flush_tx_dma_channel(struct efhw_nic *nic, uint32_t client_id,
                            uint dmaq, uint evq)
{
  struct efhw_nic_ef10ct *ef10ct = nic->arch_extra;
  struct efhw_nic_ef10ct_evq *ef10ct_evq = &ef10ct->evq[evq];
  int rc = 0;
  struct efx_auxiliary_rpc dummy;

  dummy.cmd = MC_CMD_FINI_TXQ;
  dummy.inlen = sizeof(int);
  dummy.inbuf = &dmaq;
  dummy.outlen = 0;
  rc = ef10ct_fw_rpc(nic, &dummy);

  atomic_inc(&ef10ct_evq->queues_flushing);
  schedule_delayed_work(&ef10ct_evq->check_flushes, 0);

  return rc;
}


static int
ef10ct_flush_rx_dma_channel(struct efhw_nic *nic, uint32_t client_id, uint dmaq)
{
  return -ENOSYS;
}


static int
ef10ct_translate_dma_addrs(struct efhw_nic* nic, const dma_addr_t *src,
                           dma_addr_t *dst, int n)
{
  /* All efct NICs have 1:1 mappings */
  memmove(dst, src, n * sizeof(src[0]));
  return 0;
}


/*--------------------------------------------------------------------
 *
 * Buffer table - API
 *
 *--------------------------------------------------------------------*/

static const int __ef10ct_nic_buffer_table_get_orders[] = {};

/*--------------------------------------------------------------------
 *
 * Filtering
 *
 *--------------------------------------------------------------------*/


static int
ef10ct_filter_insert(struct efhw_nic *nic, struct efx_filter_spec *spec,
                     int *rxq, unsigned pd_excl_token,
                     const struct cpumask *mask, unsigned flags)
{
  return 0;
}


static void
ef10ct_filter_remove(struct efhw_nic *nic, int filter_id)
{
}


static int
ef10ct_filter_redirect(struct efhw_nic *nic, int filter_id,
                       struct efx_filter_spec *spec)
{
  return -ENOSYS;
}


static int
ef10ct_filter_query(struct efhw_nic *nic, int filter_id,
                    struct efhw_filter_info *info)
{
  return -EOPNOTSUPP;
}


static int
ef10ct_multicast_block(struct efhw_nic *nic, bool block)
{
  return -ENOSYS;
}


static int
ef10ct_unicast_block(struct efhw_nic *nic, bool block)
{
  return -ENOSYS;
}


/*--------------------------------------------------------------------
 *
 * Device
 *
 *--------------------------------------------------------------------*/

static struct pci_dev*
ef10ct_get_pci_dev(struct efhw_nic* nic)
{
  return NULL;
}


static int
ef10ct_vi_io_region(struct efhw_nic* nic, int instance, size_t* size_out,
                    resource_size_t* addr_out)
{
  struct device *dev;
  struct efx_auxiliary_device* edev;
  struct efx_auxiliary_client* cli;
  union efx_auxiliary_param_value val;
  int rc = 0;

  EFCT_PRE(dev, edev, cli, nic, rc)
  rc = edev->ops->get_param(cli, EFX_AUXILIARY_EVQ_WINDOW, &val);
  EFCT_POST(dev, edev, cli, nic, rc);

  *size_out = val.evq_window.stride;
  *addr_out = val.evq_window.base;
  *addr_out += (instance - nic->vi_min) * val.evq_window.stride;

  return rc;
}

static int
ef10ct_design_parameters(struct efhw_nic *nic,
                         struct efab_nic_design_parameters *dp)
{
  /* Where older versions of ef_vi make assumptions about parameter values, we
   * must check that either they know about the parameter, or that the value
   * matches the assumption.
   *
   * See documentation of efab_nic_design_parameters for details of
   * compatibility issues.
   */
#define SET(PARAM, VALUE) \
  if( EFAB_NIC_DP_KNOWN(*dp, PARAM) ) \
    dp->PARAM = (VALUE);  \
  else if( (VALUE) != EFAB_NIC_DP_DEFAULT(PARAM) ) \
    return -ENODEV;

  SET(rx_superbuf_bytes, EFCT_RX_SUPERBUF_BYTES);
  SET(rx_frame_offset, EFCT_RX_HEADER_NEXT_FRAME_LOC_1 - 2);
  SET(tx_aperture_bytes, 0x1000);
  SET(tx_fifo_bytes, 0x8000);
  SET(timestamp_subnano_bits, 2);
  SET(unsol_credit_seq_mask, 0x7f);

  return 0;
}


/*--------------------------------------------------------------------
 *
 * CTPIO
 *
 *--------------------------------------------------------------------*/

static int
ef10ct_ctpio_addr(struct efhw_nic* nic, int instance, resource_size_t* addr)
{
  struct device *dev;
  struct efx_auxiliary_device* edev;
  struct efx_auxiliary_client* cli;
  union efx_auxiliary_param_value val;
  int rc;

  val.io_addr.qid_in = instance;
  EFCT_PRE(dev, edev, cli, nic, rc);
  rc = edev->ops->get_param(cli, EFX_AUXILIARY_CTPIO_WINDOW, &val);
  EFCT_POST(dev, edev, cli, nic, rc);

  /* Currently we assume throughout onload that we have a 4k region */
  if( (rc == 0) && (val.io_addr.size != 0x1000) )
    return -EOPNOTSUPP;

  if( rc == 0 )
    *addr = val.io_addr.base;

  return rc;
}

/*--------------------------------------------------------------------
 *
 * Abstraction Layer Hooks
 *
 *--------------------------------------------------------------------*/

struct efhw_func_ops ef10ct_char_functional_units = {
  .init_hardware = ef10ct_nic_init_hardware,
  .post_reset = ef10ct_nic_tweak_hardware,
  .release_hardware = ef10ct_nic_release_hardware,
  .event_queue_enable = ef10ct_nic_event_queue_enable,
  .event_queue_disable = ef10ct_nic_event_queue_disable,
  .wakeup_request = ef10ct_nic_wakeup_request,
  .accept_vi_constraints = ef10ct_accept_vi_constraints,
  .dmaq_tx_q_init = ef10ct_dmaq_tx_q_init,
  .dmaq_rx_q_init = ef10ct_dmaq_rx_q_init,
  .flush_tx_dma_channel = ef10ct_flush_tx_dma_channel,
  .flush_rx_dma_channel = ef10ct_flush_rx_dma_channel,
  .translate_dma_addrs = ef10ct_translate_dma_addrs,
  .buffer_table_orders = __ef10ct_nic_buffer_table_get_orders,
  .buffer_table_orders_num = 0,
  .filter_insert = ef10ct_filter_insert,
  .filter_remove = ef10ct_filter_remove,
  .filter_redirect = ef10ct_filter_redirect,
  .filter_query = ef10ct_filter_query,
  .multicast_block = ef10ct_multicast_block,
  .unicast_block = ef10ct_unicast_block,
  .get_pci_dev = ef10ct_get_pci_dev,
  .vi_io_region = ef10ct_vi_io_region,
  .ctpio_addr = ef10ct_ctpio_addr,
  .design_parameters = ef10ct_design_parameters,
  .max_shared_rxqs = ef10ct_max_shared_rxqs,
};

#endif
