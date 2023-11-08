/* SPDX-License-Identifier: GPL-2.0 */
/* X-SPDX-Copyright-Text: (c) Copyright 2021 Xilinx, Inc. */

#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/cacheflush.h>
#include <linux/random.h>
#include <linux/hrtimer.h>
#include <linux/mm.h>
#ifdef __has_include
#if __has_include(<linux/set_memory.h>)
#include <linux/set_memory.h>
#endif
#if __has_include(<asm/set_memory.h>)
#include <asm/set_memory.h>
#endif
#endif
#include <asm/io.h>

#include <ci/driver/ci_aux.h>
#include <ci/driver/ci_ef10ct_test.h>
#include <ci/compat.h>
#include <ci/efrm/debug_linux.h>
#include <etherfabric/internal/efct_uk_api.h>
#include <ci/tools/byteorder.h>
#include <ci/tools/bitfield.h>
#include <ci/efhw/mc_driver_pcol.h>

#include "efct_test_device.h"
#include "efct_test_ops.h"
#include "efct_test_tx.h"

#ifndef page_to_virt
/* Only RHEL7 doesn't have this macro */
#define page_to_virt(x)        __va(PFN_PHYS(page_to_pfn(x)))
#endif


static struct efx_auxiliary_client*
efct_test_open(struct auxiliary_device *adev, efx_event_handler func,
               unsigned int events_requested, void *driver_data)
{
  struct efx_auxiliary_client *client;
  struct efct_test_device *tdev;

  printk(KERN_INFO "%s\n", __func__);

  /* Currently support exactly one test device, which should be opened at most
   * once by the efct driver.
   */
  tdev = container_of(adev, struct efct_test_device, dev.auxdev);
  BUG_ON(tdev->client);

  client = kzalloc(sizeof(*client), GFP_KERNEL);
  if( !client )
    return ERR_PTR(-ENOMEM);

  client->event_handler = func;
  client->drv_priv = driver_data;
  client->net_dev = tdev->net_dev;
  tdev->client = client;
  client->tdev = tdev;

  return client;
}


static int efct_test_close(struct efx_auxiliary_client *handle)
{
  struct efct_test_device *tdev = handle->tdev;
  printk(KERN_INFO "%s\n", __func__);

  if( ! tdev )
    return -EINVAL;

  tdev->client = NULL;
  kfree(handle);

  return 0;
}


static int efct_test_ctpio_addr(struct efx_auxiliary_client *handle,
                                struct efx_auxiliary_io_addr *io)
{
  struct efct_test_device *tdev = handle->tdev;
  int txq = io->qid_in;

  printk(KERN_INFO "%s\n", __func__);

  if( tdev->txqs[txq].evq < 0 )
    return -EINVAL;

  io->base = virt_to_phys(tdev->txqs[txq].ctpio);
  io->size = 0x1000;
  return 0;
}


static int efct_test_get_param(struct efx_auxiliary_client *handle,
                               enum efx_auxiliary_param p,
                               union efx_auxiliary_param_value *arg)
{
  int rc = -ENOSYS;

  printk(KERN_INFO "%s: param %d\n", __func__, p);

  switch(p) {
   case EFX_AUXILIARY_NETDEV:
    arg->net_dev = handle->net_dev;
    rc = 0;
    break;
   case EFX_AUXILIARY_VARIANT:
    arg->variant = 'T';
    rc = 0;
    break;
   case EFX_AUXILIARY_REVISION:
    arg->value = 1;
    rc = 0;
    break;
   case EFX_AUXILIARY_NIC_RESOURCES:
    arg->nic_res.evq_min = 0;
    arg->nic_res.evq_lim = EFCT_TEST_EVQS_N - 1;
    arg->nic_res.txq_min = 0;
    arg->nic_res.txq_lim = EFCT_TEST_TXQS_N - 1;
    rc = 0;
    break;
   case EFX_AUXILIARY_EVQ_WINDOW:
    arg->evq_window.base = virt_to_phys(handle->tdev->evq_window);
    arg->evq_window.stride = 0x1000;
    rc = 0;
    break;
   case EFX_AUXILIARY_CTPIO_WINDOW:
    rc = efct_test_ctpio_addr(handle, &arg->io_addr);
    break;
   default:
    break;
  };

  return rc;
}


static int efct_test_set_param(struct efx_auxiliary_client *handle,
                               enum efx_auxiliary_param p,
                               union efx_auxiliary_param_value *arg)
{
  int rc = -ENOSYS;

  printk(KERN_INFO "%s: param %d\n", __func__, p);

  return rc;
}


static int efct_test_init_evq(struct efx_auxiliary_client *handle,
                              struct efx_auxiliary_evq_params *params)
{
  struct efct_test_evq *evq = &handle->tdev->evqs[params->qid];

  printk(KERN_INFO "%s: qid %d\n", __func__, params->qid);
  if( evq->inited )
    return -EBUSY;

  BUG_ON(evq->txqs != 0);

  evq->inited = true;
  evq->q_base = page_to_virt(params->q_page);
  evq->entries = params->entries;
  evq->ptr = 0;
  evq->mask = evq->entries - 1;

  return 0;
}


static void efct_test_free_evq(struct efx_auxiliary_client *handle, int evq)
{
  printk(KERN_INFO "%s: qid %d\n", __func__, evq);
  WARN(!handle->tdev->evqs[evq].inited,
       "%s: Error freeing q %d but not inited\n", __func__, evq);

  WARN(handle->tdev->evqs[evq].txqs != 0,
       "%s: Error freeing evq %d, but still bound to txqs %x\n",
       __func__, evq, handle->tdev->evqs[evq].txqs);

  handle->tdev->evqs[evq].inited = false;
}


static int efct_test_init_txq(struct efx_auxiliary_client *handle,
                              struct efx_auxiliary_txq_params *params)
{
  struct efct_test_device *tdev = handle->tdev;
  struct efct_test_txq *txq;
  int txq_idx = -1;
  int i;
  int evq = params->evq;

  printk(KERN_INFO "%s: evq %d\n", __func__, evq);
  if( !tdev->evqs[evq].inited )
    return -EINVAL;

  /* Onload allocate vis (and hence EVQs) through a buddy allocator, so we can
   * just allocate linearly and should end up testing differing EVQ and TXQ
   * ids.
   */
  for( i = 0; i < EFCT_TEST_TXQS_N; i++ )
    if( tdev->txqs[i].evq < 0 ) {
      txq_idx = i;
      break;
    }

  if( txq_idx < 0 )
    return -EBUSY;

  txq = &tdev->txqs[txq_idx];

  txq->ctpio = kmalloc(0x1000, GFP_KERNEL);
  if( !txq->ctpio )
    return -ENOMEM;
  memset(txq->ctpio, 0xff, 0x1000);
  set_memory_wc((unsigned long)txq->ctpio, 1);

  atomic_set(&txq->timer_running, 1);
  INIT_DELAYED_WORK(&txq->timer, efct_test_tx_timer);
  schedule_delayed_work(&txq->timer, 100);

  txq->evq = evq;
  txq->tdev = tdev;
  tdev->evqs[evq].txqs |= 1 << txq_idx;
  txq->ptr = 0;
  txq->pkt_ctr = 0;

  printk(KERN_INFO "%s: bound txq %d to evq %d\n", __func__, txq_idx,
         evq);

  return txq_idx;
}


static void efct_test_free_txq(struct efx_auxiliary_client *handle, int txq_idx)
{
  struct efct_test_device *tdev = handle->tdev;
  int evq = tdev->txqs[txq_idx].evq;
  struct efct_test_txq *txq = &tdev->txqs[txq_idx];

  printk(KERN_INFO "%s: txq %d\n", __func__, txq_idx);
  WARN(evq < 0,
       "%s: Error: freeing q %d, but not bound to evq\n", __func__, txq_idx);

  atomic_set(&txq->timer_running, 0);
  cancel_delayed_work_sync(&txq->timer);

  evq_push_tx_flush_complete(&tdev->evqs[evq], txq_idx);

  tdev->evqs[evq].txqs &= ~(1 << txq_idx);
  txq->evq = -1;
  set_memory_wb((unsigned long)txq->ctpio, 1);

  kfree(txq->ctpio);
}


static int efct_test_fw_rpc(struct efx_auxiliary_client *handle,
                            struct efx_auxiliary_rpc *rpc)
{
  int rc;

  switch(rpc->cmd) {
   case MC_CMD_INIT_EVQ:
    if( rpc->inlen != sizeof(struct efx_auxiliary_evq_params) )
      rc = -EINVAL;
    else
      rc = efct_test_init_evq(handle,
                             (struct efx_auxiliary_evq_params *)rpc->inbuf);
    break;
   case MC_CMD_FINI_EVQ:
    if( rpc->inlen != sizeof(int) ) {
      rc = -EINVAL;
    }
    else {
      efct_test_free_evq(handle, *rpc->inbuf);
      rc = 0;
    }
    break;
   case MC_CMD_INIT_TXQ:
    if( rpc->inlen != sizeof(struct efx_auxiliary_txq_params) )
      rc = -EINVAL;
    else
      rc = efct_test_init_txq(handle,
                             (struct efx_auxiliary_txq_params *)rpc->inbuf);
    break;
   case MC_CMD_FINI_TXQ:
    if( rpc->inlen != sizeof(int) ) {
      rc = -EINVAL;
    }
    else {
      efct_test_free_txq(handle, *rpc->inbuf);
      rc = 0;
    }
    break;
   default:
    rc = -ENOSYS;
  };

  printk(KERN_INFO "%s: cmd %d rc %d\n", __func__, rpc->cmd, rc);
  return rc;
}


int efct_test_queues_alloc(struct efx_auxiliary_client *handle,
			   struct efx_auxiliary_queues_alloc_params *params)
{
  return -ENOSYS;
}


int efct_test_queues_free(struct efx_auxiliary_client *handle,
			  struct efx_auxiliary_queues_alloc_params *params)
{
  return -ENOSYS;
}


const struct efx_auxiliary_devops test_devops = {
  .open = efct_test_open,
  .close = efct_test_close,
  .get_param = efct_test_get_param,
  .set_param = efct_test_set_param,
  .fw_rpc = efct_test_fw_rpc,
  .queues_alloc = efct_test_queues_alloc,
  .queues_free = efct_test_queues_free,
};

