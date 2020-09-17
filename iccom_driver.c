/*
 * Copyright (c) 2016, Renesas Electronics Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/of_address.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/version.h>

#include "iccom.h"
#include "iccom_core.h"
#include "iccom_driver.h"

static int32_t iccom_drv_open(struct iccom *iccom,
				struct iccom_channel **channel_out);
static ssize_t iccom_drv_read(struct iccom_channel *channel,
				struct iccom_cmd *cmd);
static ssize_t iccom_drv_write(struct iccom_channel *channel,
				struct iccom_cmd *cmd);
static int32_t iccom_drv_ioctl(struct iccom_channel *channel,
				int32_t req, void *arg);
static int32_t iccom_drv_close(struct iccom_channel *channel);
static irqreturn_t iccom_drv_isr(int irq, void *dev_id);
static int32_t iccom_drv_write_mfis(struct iccom_channel *channel,
				uint32_t write_data, size_t msg_size);
static int32_t iccom_drv_poll_int_clear(uint32_t *iicr_add, int32_t timer);
static int32_t iccom_drv_probe(struct platform_device *pdev);
static int32_t iccom_drv_remove(struct platform_device *pdev);
static int32_t iccom_drv_init(void);
static void iccom_drv_exit(void);
static void iccom_drv_device_release(struct device *dev);
static void iccom_drv_release_channel(struct iccom_channel *channel);
static void iccom_drv_release_iccom(struct iccom *iccom, uint8_t request_flg);

static int32_t iccom_drv_poll_int_clear(uint32_t *iicr_add, int32_t timer)
{
	int32_t i;
	uint32_t check_mfis;
	int32_t ret;

	pr_debug("%s: Start\n", __func__);

	for (i = 0; i < timer; i++) {
		check_mfis = ioread32(iicr_add);
		if ((check_mfis & ICCOM_CTRL_INT) == 0U) {
			break;
		}
		usleep_range(ICCOM_WAIT_TIMER_US, ICCOM_WAIT_TIMER_US + 1U);
	}

	if (i < timer) {
		ret = ICCOM_OK;
	} else {
		ret = -EDEADLK;
	}

	pr_debug("%s: End ret = %d, count = %d\n", __func__, ret, i);

	return ret;
}

static int32_t iccom_drv_write_mfis(struct iccom_channel *channel,
				uint32_t write_data, size_t msg_size)
{
	int32_t ret;
	struct iccom_communication_info *iccom_comm;
	struct iccom_hardware_info *iccom_hw;

	pr_debug("%s: Start write_data = 0x%x, msg_size = %zd\n",
		__func__, write_data, msg_size);

	iccom_comm = (struct iccom_communication_info *)channel->priv;
	iccom_hw = (struct iccom_hardware_info *)channel->iccom->priv;

	mutex_lock(&iccom_comm->mutex_poll_intbit);

	ret = iccom_drv_poll_int_clear(iccom_comm->mfis_iicr_addr,
					iccom_hw->trg_timeout);
	if (ret == ICCOM_OK) {
		iowrite32(msg_size, iccom_comm->mfis_imbr_addr);
		iowrite32(write_data, iccom_comm->mfis_iicr_addr);
	}

	mutex_unlock(&iccom_comm->mutex_poll_intbit);

	pr_debug("%s: End ret = %d\n", __func__, ret);

	return ret;
}

static int32_t iccom_drv_open(struct iccom *iccom,
				struct iccom_channel **channel_out)
{
	int32_t ret = ICCOM_OK;
	struct iccom_channel *l_channel = NULL;
	struct iccom_communication_info *iccom_comm = NULL;
	struct iccom_hardware_info *iccom_hw;
	uint32_t write_iicr;
	uint32_t copy_eicr;
	void __iomem *remap_addr;

	dev_dbg(iccom->dev, "%s: Start\n", __func__);

	mutex_lock(&iccom->mutex_channel_open);
	if ((iccom->flg & ICCOM_FLG_CH_OPEN) == 0U) {
		pr_debug("Open route\n");
		iccom->flg |= ICCOM_FLG_CH_OPEN;
		mutex_unlock(&iccom->mutex_channel_open);
		l_channel = (struct iccom_channel *)devm_kzalloc(
			iccom->dev,
			sizeof(struct iccom_channel) +
			sizeof(struct iccom_communication_info),
			GFP_KERNEL);
		if (l_channel == NULL) {
			ret = -ENOMEM;
			dev_err(iccom->dev, "[Err]No enough memory\n");
		}
	} else {
		ret = -EBUSY;
		dev_err(iccom->dev, "[Err]This channel already open\n");
		mutex_unlock(&iccom->mutex_channel_open);
	}

	if (ret == ICCOM_OK) {
		l_channel->iccom = iccom;
		l_channel->priv = &l_channel[1];

		iccom_comm = (struct iccom_communication_info *)l_channel->priv;
		iccom_hw = (struct iccom_hardware_info *)iccom->priv;

		iccom_comm->read_status = ICCOM_READ_STATE_WAIT_READ;
		iccom_comm->write_status[ICCOM_CTA_UPPER] =
						ICCOM_WRITE_STATE_WAIT_WRITE;
		iccom_comm->write_status[ICCOM_CTA_BOTTOM] =
						ICCOM_WRITE_STATE_WAIT_WRITE;

		(void)memset(&iccom_comm->ack_arrive, 0,
				sizeof(iccom_comm->ack_arrive));
		(void)memset(&iccom_comm->recv_index, 0,
				sizeof(iccom_comm->recv_index));
		(void)memset(&iccom_comm->recv_eicr, 0,
				sizeof(iccom_comm->recv_eicr));
		(void)memset(&iccom_comm->recv_embr, 0,
				sizeof(iccom_comm->recv_embr));
		(void)memset(&iccom_comm->ack_embr, 0,
				sizeof(iccom_comm->ack_embr));
		(void)memset(&iccom_comm->cta_flg, 0,
				sizeof(iccom_comm->cta_flg));

		sema_init(&iccom_comm->sem_send_cta, ICCOM_CTA_SBUF_NUM);
		mutex_init(&iccom_comm->mutex_poll_intbit);
		spin_lock_init(&iccom_comm->spinlock_mfis_info);

		spin_lock_init(&iccom_comm->spinlock_cta_flg);

		init_waitqueue_head(&iccom_comm->write_q);
		init_waitqueue_head(&iccom_comm->read_q);
		init_waitqueue_head(&iccom_comm->ioctl_q);
		iccom_comm->close_flg = 0;

		remap_addr = ioremap_nocache(
			(uintptr_t)iccom_hw->mfis_iicr_addr, ICCOM_MFIS_LEN);
		if (remap_addr == NULL) {
			dev_err(iccom->dev, "[Err]Remap IICR failed\n");
			ret = -ENOMEM;
		} else {
			iccom_comm->mfis_iicr_addr = (uint32_t *)remap_addr;
		}

		remap_addr = ioremap_nocache(
			(uintptr_t)iccom_hw->mfis_eicr_addr, ICCOM_MFIS_LEN);
		if (remap_addr == NULL) {
			dev_err(iccom->dev, "[Err]Remap EICR failed\n");
			ret = -ENOMEM;
		} else {
			iccom_comm->mfis_eicr_addr = (uint32_t *)remap_addr;
		}

		remap_addr = ioremap_nocache(
			(uintptr_t)iccom_hw->mfis_imbr_addr, ICCOM_MFIS_LEN);
		if (remap_addr == NULL) {
			dev_err(iccom->dev, "[Err]Remap IMBR failed\n");
			ret = -ENOMEM;
		} else {
			iccom_comm->mfis_imbr_addr = (uint32_t *)remap_addr;
		}

		remap_addr = ioremap_nocache(
			(uintptr_t)iccom_hw->mfis_embr_addr, ICCOM_MFIS_LEN);
		if (remap_addr == NULL) {
			dev_err(iccom->dev, "[Err]Remap EMBR failed\n");
			ret = -ENOMEM;
		} else {
			iccom_comm->mfis_embr_addr = (uint32_t *)remap_addr;
		}

		remap_addr = ioremap_nocache((uintptr_t)iccom_hw->cta_addr,
				iccom_hw->cta_size);
		if (remap_addr == NULL) {
			dev_err(iccom->dev, "[Err]Remap CTA failed\n");
			ret = -ENOMEM;
		} else {
			iccom_comm->cta_addr = (uint8_t *)remap_addr;
		}
	}

	if (ret == ICCOM_OK) {
		dev_dbg(iccom->dev, "flg check: 0x%X\n",
			(iccom->flg & ICCOM_FLG_CH_INIT_END));
		if ((iccom->flg & ICCOM_FLG_CH_INIT_END) == 0U) {
			write_iicr =
				(ICCOM_CTRL_INIT_END | ICCOM_CTRL_INT);
			iowrite32(write_iicr,
				iccom_comm->mfis_iicr_addr);

			mutex_lock(&iccom_comm->mutex_poll_intbit);
			ret = iccom_drv_poll_int_clear(
				iccom_comm->mfis_iicr_addr,
				iccom_hw->trg_timeout);
			mutex_unlock(&iccom_comm->mutex_poll_intbit);
			if (ret == ICCOM_OK) {
				iccom->flg |= ICCOM_FLG_CH_INIT_END;
			} else {
				ret = -EDEADLK;
				dev_err(iccom->dev,
					"[Err] Channel init EDEADLK\n");
			}
		}
	}

	if (ret == ICCOM_OK) {
		*channel_out = l_channel;
		iccom->channel = l_channel;

		/*
		 * Discard the interrupt received prior to the
		 * open process is completed.
		 */
		copy_eicr = ioread32(iccom_comm->mfis_eicr_addr);
		iowrite32((copy_eicr & ICCOM_CTRL_CLEAR_INIT_BIT),
			iccom_comm->mfis_eicr_addr);
		enable_irq(iccom_hw->irq_no);

		dev_dbg(iccom->dev, "iccom_communication_info\n");
		dev_dbg(iccom->dev, "mfis_iicr_addr = %p\n",
			iccom_comm->mfis_iicr_addr);
		dev_dbg(iccom->dev, "mfis_eicr_addr = %p\n",
			iccom_comm->mfis_eicr_addr);
		dev_dbg(iccom->dev, "mfis_imbr_addr = %p\n",
			iccom_comm->mfis_imbr_addr);
		dev_dbg(iccom->dev, "mfis_embr_addr = %p\n",
			iccom_comm->mfis_embr_addr);
		dev_dbg(iccom->dev, "cta_addr = %p\n", iccom_comm->cta_addr);
	} else {
		if (ret != -EBUSY) {
			mutex_lock(&iccom->mutex_channel_open);
			iccom->flg &= ~ICCOM_FLG_CH_OPEN;
			mutex_unlock(&iccom->mutex_channel_open);
			if (l_channel != NULL) {
				iccom_drv_release_channel(l_channel);
			}
		}
	}
	dev_dbg(iccom->dev, "%s: End ret = %d\n", __func__, ret);

	return ret;
}

static ssize_t iccom_drv_read(struct iccom_channel *channel,
				struct iccom_cmd *cmd)
{
	struct iccom_communication_info *iccom_comm;
	int32_t ret_poll;
	uint32_t set_info;
	enum iccom_cta_area cta_sw;
	int32_t err_wait;
	uintptr_t irq_flg = 0UL;
	ssize_t ret = ICCOM_OK;
	uint8_t *tgt_addr;
	int64_t ret_cpy = 0;
	int32_t ret_access;
	uint32_t recv_i;

	pr_debug("%s: Start\n", __func__);
	iccom_comm = (struct iccom_communication_info *)channel->priv;

	if (iccom_comm->read_status == ICCOM_READ_STATE_SEND_ACK) {
		pr_debug("Send ACK\n");
		cta_sw = iccom_comm->recv_index[0];
		set_info = (cta_sw << ICCOM_SHIFT_SET_ACK_AREA);
		set_info |= ICCOM_CTRL_INT;

		/* Organize MFIS information */
		spin_lock_irqsave(&iccom_comm->spinlock_mfis_info, irq_flg);
		for (recv_i = 0U; recv_i < (ICCOM_CTA_RBUF_NUM - 1); recv_i++) {
			iccom_comm->recv_index[recv_i] =
				iccom_comm->recv_index[recv_i + 1U];
		}
		iccom_comm->data_arrive--;
		spin_unlock_irqrestore(&iccom_comm->spinlock_mfis_info,
					irq_flg);

		ret_poll = iccom_drv_write_mfis(channel, set_info,
			iccom_comm->recv_embr[cta_sw]);
		if (ret_poll != ICCOM_OK) {
			iccom_comm->read_status = ICCOM_READ_STATE_WAIT_READ;
			ret = -EDEADLK;
			dev_err(channel->iccom->dev, "[Err]Send Ack failed\n");
		}
	}

	if (ret == ICCOM_OK) {
		if (iccom_comm->data_arrive == 0U) {
			iccom_comm->read_status = ICCOM_READ_STATE_RECEIVING;
		}
		pr_debug("Wait read\n");
		err_wait = wait_event_interruptible(iccom_comm->read_q,
			((iccom_comm->data_arrive >= 1) ||
				iccom_comm->close_flg == 1));
		if (err_wait < 0) {
			ret = -EINTR;
		} else if (iccom_comm->data_arrive == 0) {
			pr_debug("read close flg = %d\n",
				iccom_comm->close_flg);
			/* Iccom_lib_Final() is called and all ACK is sended. */
			iccom_comm->close_flg = 2;
			wake_up_interruptible(&iccom_comm->ioctl_q);
			ret = -ECANCELED;
		} else {
			cta_sw = iccom_comm->recv_index[0];
			cmd->count = iccom_comm->recv_embr[cta_sw];
			tgt_addr = (iccom_comm->cta_addr +
				((cta_sw + ICCOM_CTA_SBUF_NUM) *
					ICCOM_BUF_MAX_SIZE));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
			ret_access = access_ok(cmd->buf, cmd->count);
#else
			ret_access = access_ok(VERIFY_WRITE, cmd->buf,
					cmd->count);
#endif
			if (ret_access != 0) {
				dev_dbg(channel->iccom->dev,
					"copy_to_user route\n");
				ret_cpy = copy_to_user(cmd->buf, tgt_addr,
							cmd->count);
			} else {
				dev_dbg(channel->iccom->dev,
					"memcpy route\n");
				memcpy_fromio(cmd->buf, tgt_addr, cmd->count);
			}

			if (ret_cpy == 0) {
				iccom_comm->read_status =
					ICCOM_READ_STATE_SEND_ACK;
				ret = cmd->count;
			}
		}
	}

	pr_debug("%s: End ret = %zd\n", __func__, ret);

	return ret;
}

static ssize_t iccom_drv_write(struct iccom_channel *channel,
				struct iccom_cmd *cmd)
{
	struct iccom_communication_info *iccom_comm;
	struct iccom_hardware_info *iccom_hw;
	int32_t ret_pall;
	enum iccom_cta_area cta_sw = ICCOM_CTA_UPPER;
	uint32_t mfis_set_info;
	int32_t err;
	ssize_t ret = 0;
	int64_t ret_cpy = 0;
	uint8_t *tgt_addr;
	enum iccom_cta_area snd_area;
	uintptr_t irq_flg = 0UL;

	pr_debug("%s: Start cmd->cnt = %zd\n", __func__, cmd->count);

	iccom_comm = (struct iccom_communication_info *)channel->priv;
	iccom_hw = (struct iccom_hardware_info *)channel->iccom->priv;

	if (down_trylock(&iccom_comm->sem_send_cta) != 0) {
		pr_err("[Err]No CTA semaphore\n");
		ret = -ENOSPC;
	} else {
		spin_lock_irqsave(&iccom_comm->spinlock_cta_flg, irq_flg);
		for (snd_area = 0; snd_area < ICCOM_CTA_COUNT; snd_area++) {
			if ((iccom_comm->cta_flg[snd_area] == 0U) &&
				(iccom_comm->write_status[snd_area] ==
						ICCOM_WRITE_STATE_WAIT_WRITE)) {
				cta_sw = snd_area;
				iccom_comm->cta_flg[cta_sw] = 1U;
				break;
			}
		}
		spin_unlock_irqrestore(&iccom_comm->spinlock_cta_flg, irq_flg);

		if (snd_area >= ICCOM_CTA_COUNT) {
			pr_err("[Err]CTA area is all used\n");
			ret = -ENOSPC;
		} else {
			pr_debug("CTA%d write\n", cta_sw);

			tgt_addr = (iccom_comm->cta_addr +
				(cta_sw * ICCOM_BUF_MAX_SIZE));
			dev_dbg(channel->iccom->dev,
				"memcpy_toio : "
				"tgt_addr = %p, buf = %p,"
				" count = %lu\n", (void *)tgt_addr,
				(void *)cmd->buf, cmd->count);
				memcpy_toio(tgt_addr, cmd->buf, cmd->count);

			if (ret_cpy == 0) {
				dev_dbg(channel->iccom->dev, "Write success\n");
				mfis_set_info = ((cta_sw
						<< ICCOM_SHIFT_SET_SEND_AREA)
						| (ICCOM_CTRL_INT
						| ICCOM_CTRL_SND_ACK));
				pr_debug("mfis set info :0x%X\n",
						mfis_set_info);
				iccom_comm->ack_arrive[cta_sw] = 0U;
				iccom_comm->write_status[cta_sw] =
						ICCOM_WRITE_STATE_RECV_ACK;
				ret_pall = iccom_drv_write_mfis(channel,
						mfis_set_info, cmd->count);
				if (ret_pall != ICCOM_OK) {
					dev_err(channel->iccom->dev,
						"[Err]Write EDEADLK\n");
					spin_lock_irqsave(
						&iccom_comm->spinlock_cta_flg,
						irq_flg);
					iccom_comm->cta_flg[cta_sw] = 0U;
					spin_unlock_irqrestore(
						&iccom_comm->spinlock_cta_flg,
						irq_flg);
					ret = -EDEADLK;
				} else {
					pr_debug("Wait ack\n");
					err = wait_event_interruptible_timeout(
						iccom_comm->write_q,
						(iccom_comm->ack_arrive[cta_sw]
							== 1),
						iccom_hw->ack_timeout_jf);
					if (err == 0) {
						ret = -ETIMEDOUT;
					} else if (err < 0) {
						ret = -EINTR;
					} else {
						ret
						 = iccom_comm->ack_embr[cta_sw];
					}
				}
				iccom_comm->write_status[cta_sw] =
						ICCOM_WRITE_STATE_WAIT_WRITE;
			} else {
				spin_lock_irqsave(&iccom_comm->spinlock_cta_flg,
							irq_flg);
				iccom_comm->cta_flg[cta_sw] = 0U;
				spin_unlock_irqrestore(
						&iccom_comm->spinlock_cta_flg,
						irq_flg);
			}
		}
		up(&iccom_comm->sem_send_cta);
	}

	pr_debug("%s: End ret = %zd\n", __func__, ret);

	return ret;
}

static int32_t iccom_drv_ioctl(struct iccom_channel *channel,
				int32_t req, void *arg)
{
	int32_t ret = 0;
	struct iccom_communication_info *iccom_comm;
	struct iccom_hardware_info *iccom_hw;
	int32_t err_wait;

	pr_debug("%s: Start\n", __func__);

	iccom_comm = (struct iccom_communication_info *)channel->priv;
	iccom_hw = (struct iccom_hardware_info *)channel->iccom->priv;

	switch (req) {
	case ICCOM_IOC_CANCEL_RECEIVE:
		disable_irq(iccom_hw->irq_no);
		iccom_comm->close_flg = 1;
		wake_up_interruptible(&iccom_comm->read_q);
		/* Wait ACK send finish or trg timeout */
		err_wait = wait_event_interruptible_timeout(
					iccom_comm->ioctl_q,
					(iccom_comm->close_flg == 2),
					iccom_hw->trg_timeout);
		if (err_wait == 0) {
			pr_err("[Err]Wait all ACK send failed.\n");
			ret = -ETIMEDOUT;
		}
		break;
	default:
		ret = -EINVAL;
		pr_err("[Err]Invalid argument. req=%d\n", req);
		break;
	}

	pr_debug("%s: End ret = %d\n", __func__, ret);

	return ret;
}

static int32_t iccom_drv_close(struct iccom_channel *channel)
{
	struct iccom *l_iccom;
	struct iccom_communication_info *iccom_comm;
	struct iccom_hardware_info *iccom_hw;
	uintptr_t irq_flg = 0UL;

	pr_debug("%s: Start\n", __func__);

	l_iccom = channel->iccom;
	iccom_comm = (struct iccom_communication_info *)channel->priv;
	iccom_hw = (struct iccom_hardware_info *)l_iccom->priv;

	if (iccom_comm->close_flg == 0) {
		pr_debug("%s: disable_irq exec . iccom_comm->close_flg = %u\n",
			__func__, iccom_comm->close_flg);
		/* Process finalize but not call Iccom_lib_Final(). */
		disable_irq(iccom_hw->irq_no);
	}

	spin_lock_irqsave(&iccom_comm->spinlock_mfis_info, irq_flg);
	(void)memset(&iccom_comm->recv_index, 0,
		sizeof(iccom_comm->recv_index));
	(void)memset(&iccom_comm->recv_embr, 0, sizeof(iccom_comm->recv_embr));
	iccom_comm->data_arrive = 0;
	spin_unlock_irqrestore(&iccom_comm->spinlock_mfis_info, irq_flg);
	wake_up_interruptible(&iccom_comm->read_q);
	l_iccom->channel = NULL;

	iccom_drv_release_channel(channel);
	l_iccom->flg &= ~ICCOM_FLG_CH_OPEN;

	pr_debug("%s: End\n", __func__);

	return ICCOM_OK;
}

/* Interrupt handler */
static irqreturn_t iccom_drv_isr(int irq, void *dev_id)
{
	uint32_t mfis_ctrl;
	uint32_t mfis_msg;
	enum iccom_cta_area cta_sw;
	uint32_t snd_ack_flg;
	struct iccom_communication_info *iccom_comm = NULL;
	struct iccom_channel *l_channel;
	struct iccom *l_iccom;
	uintptr_t irq_flg = 0UL;

	pr_debug("%s: Start\n", __func__);

	disable_irq_nosync(irq);

	l_iccom = (struct iccom *)dev_id;
	l_channel = l_iccom->channel;
	/*
	 * If the close function is called by the present function after
	 * it has been called, to check here for iccom_channel there is a
	 * possibility of NULL
	 */
	if (l_channel != NULL) {
		iccom_comm = (struct iccom_communication_info *)l_channel->priv;
	}
	/*
	 * If the close function is called by the present function after
	 * it has been called, to check here for iccom_communication_info
	 * there is a possibility of NULL
	 */
	if (iccom_comm != NULL) {
		mfis_ctrl = ioread32(iccom_comm->mfis_eicr_addr);

		pr_debug("mfis ctrl :0x%X\n", mfis_ctrl);
		snd_ack_flg = mfis_ctrl & ICCOM_CTRL_SND_ACK;
		mfis_msg = ioread32(iccom_comm->mfis_embr_addr);

		if (snd_ack_flg == 0U) {
			cta_sw = (mfis_ctrl & ICCOM_CTRL_ACK_BUFF)
				>> ICCOM_SHIFT_GET_ACK_AREA;
			pr_debug("Recv Ack\n");
			/* Check target message is not timeout. */
			spin_lock_irqsave(&iccom_comm->spinlock_cta_flg,
						irq_flg);
			iccom_comm->cta_flg[cta_sw] = 0U;
			spin_unlock_irqrestore(&iccom_comm->spinlock_cta_flg,
						irq_flg);

			if (iccom_comm->write_status[cta_sw]
			     == ICCOM_WRITE_STATE_RECV_ACK) {
					iccom_comm->ack_arrive[cta_sw] = 1U;
					iccom_comm->ack_embr[cta_sw] = mfis_msg;
					wake_up_interruptible(
						&iccom_comm->write_q);
			} else {
				pr_err("[Err]No wait Area \n");
			}
		} else {
			cta_sw = (mfis_ctrl & ICCOM_CTRL_SND_BUFF)
				>> ICCOM_SHIFT_GET_RECV_AREA;
			pr_debug("Recv Msg\n");
			/* Check receiving message rule. */
			if (iccom_comm->data_arrive < ICCOM_CTA_RBUF_NUM) {
				/* Save MFIS info */
				spin_lock_irqsave(
					&iccom_comm->spinlock_mfis_info,
					irq_flg);
				iccom_comm->recv_index[iccom_comm->data_arrive]
					= cta_sw;
				iccom_comm->data_arrive++;
				spin_unlock_irqrestore(
					&iccom_comm->spinlock_mfis_info,
					irq_flg);
				iccom_comm->recv_eicr[cta_sw] = mfis_ctrl;
				iccom_comm->recv_embr[cta_sw] = mfis_msg;
				wake_up_interruptible(&iccom_comm->read_q);
			}
		}
		iowrite32((mfis_ctrl & ICCOM_CTRL_CLEAR_INIT_BIT),
			iccom_comm->mfis_eicr_addr);
	}
	enable_irq(irq);

	pr_debug("%s: End\n", __func__);

	return IRQ_HANDLED;
}

const struct iccom_ops iccom_drv_fops = {
	.owner		= THIS_MODULE,
	.open		= &iccom_drv_open,
	.read		= &iccom_drv_read,
	.write		= &iccom_drv_write,
	.ioctl		= &iccom_drv_ioctl,
	.close		= &iccom_drv_close,
};

static const struct of_device_id iccom_drv_match[] = {
	{ .compatible = "renesas,iccom-rcar", },
	{ }
};
MODULE_DEVICE_TABLE(of, iccom_drv_match);

static int32_t iccom_drv_probe(struct platform_device *pdev)
{
	struct iccom *l_iccom = NULL;
	struct iccom_hardware_info *iccom_hw = NULL;
	struct resource mfis_base;
	struct resource *req_iicr;
	struct resource *req_eicr;
	struct resource *req_imbr;
	struct resource *req_embr;
	struct resource *req_cta;
	int32_t ret = ICCOM_OK;
	int32_t ret_irq;
	int32_t ret_get_irq;
	const void *reg_offset_ptr;
	const void *cta_addr_ptr;
	const void *ack_timer_ptr;
	const void *trg_timer_ptr;
	struct device_node *dev_node;
	int8_t channel_name[ICCOM_CANNEL_NO_LEN];
	int32_t len;
	enum Iccom_channel_number i;
	enum Iccom_channel_number l_channel_no = ICCOM_CHANNEL_MAX;
	size_t fixed_len;
	uint8_t request_flg = 0U;
	int32_t ret_cmp;

	pr_debug("%s: Start\n", __func__);

	dev_node = dev_of_node(&pdev->dev);
	if (dev_node != NULL) {
		fixed_len = strlen(dev_node->name);
		for (i = ICCOM_CHANNEL_0; i < ICCOM_CHANNEL_MAX; i++) {
			(void)snprintf(channel_name, ICCOM_CANNEL_NO_LEN,
					"%d", i);
			ret_cmp = strncmp(&dev_node->name[fixed_len - 1U],
				channel_name, strlen(channel_name));
			if (ret_cmp == 0) {
				l_channel_no = i;
				break;
			}
		}
		if (i >= ICCOM_CHANNEL_MAX) {
			ret = -ENODEV;
			dev_err(&pdev->dev, "Get Channel No failed\n");
		}

		if (ret == ICCOM_OK) {
			l_iccom = iccom_core_alloc(&pdev->dev,
						(int32_t)l_channel_no,
						&iccom_drv_fops,
						sizeof(*iccom_hw));
			if (l_iccom == NULL) {
				ret = -ENOMEM;
				pr_err("[Err]iccom_core_alloc err\n");
			}
		}

		if (ret == ICCOM_OK) {
			iccom_hw =
				(struct iccom_hardware_info *)l_iccom->priv;
			ret_get_irq = platform_get_irq(pdev, 0U);
			if (ret_get_irq <= 0) {
				ret = ret_get_irq;
				pr_err("[Err]Get_irq err\n");
			} else {
				iccom_hw->irq_no = (uint32_t)ret_get_irq;
			}
		}

		if (ret == ICCOM_OK) {
			ret = of_address_to_resource(dev_node, 0, &mfis_base);
			if (ret != 0) {
				dev_err(l_iccom->dev,
					"Get MFIS base failed\n");
			}
		}

		if (ret == ICCOM_OK) {
			reg_offset_ptr = of_get_property(dev_node,
							"iccom,reg-offset",
							&len);
			if ((reg_offset_ptr == NULL) ||
				(len < ICCOM_REG_OFFSET_LEN)) {
				ret = -ENODEV;
				dev_err(l_iccom->dev,
					"No defined reg_offset\n");
			}

			cta_addr_ptr = of_get_property(dev_node,
							"iccom,cta-memory",
							&len);
			if ((cta_addr_ptr == NULL) ||
				(len < ICCOM_CTA_MEMORY_LEN)) {
				ret = -ENODEV;
				dev_err(l_iccom->dev,
					"No defined cta_memory\n");
			}

			ack_timer_ptr = of_get_property(dev_node,
							"iccom,ack-timeout",
							NULL);
			if (ack_timer_ptr == NULL) {
				ret = -ENODEV;
				dev_err(l_iccom->dev,
					"No defined ack_timeout\n");
			}

			trg_timer_ptr = of_get_property(dev_node,
							"iccom,trg-timeout",
							NULL);
			if (trg_timer_ptr == NULL) {
				ret = -ENODEV;
				dev_err(l_iccom->dev,
					"No defined trg_timeout\n");
			}
		}

		if (ret == ICCOM_OK) {
			iccom_hw->mfis_iicr_addr =
				mfis_base.start + be32_to_cpup(reg_offset_ptr);
			iccom_hw->mfis_eicr_addr =
					mfis_base.start +
					be32_to_cpup(reg_offset_ptr + 4);
			iccom_hw->mfis_imbr_addr =
					mfis_base.start +
					be32_to_cpup(reg_offset_ptr + 8);
			iccom_hw->mfis_embr_addr =
					mfis_base.start +
					be32_to_cpup(reg_offset_ptr + 12);

			iccom_hw->cta_addr = be32_to_cpup(cta_addr_ptr + 4);
			iccom_hw->cta_size = be32_to_cpup(cta_addr_ptr + 12);

			iccom_hw->trg_timeout = be32_to_cpup(trg_timer_ptr);
			iccom_hw->ack_timeout_jf =
					msecs_to_jiffies(
						be32_to_cpup(ack_timer_ptr));

			ret_irq = request_irq(iccom_hw->irq_no,
						(irq_handler_t)&iccom_drv_isr,
						IRQF_PROBE_SHARED,
						l_iccom->name,
						(void *)l_iccom);

			if (ret_irq != 0) {
				ret = -ENODEV;
				dev_err(l_iccom->dev, "SAME IRQ registered\n");
			} else {
				request_flg |= ICCOM_REQUEST_IRQ;
				disable_irq_nosync(iccom_hw->irq_no);
			}

			req_iicr = request_mem_region(
					(uintptr_t)iccom_hw->mfis_iicr_addr,
					ICCOM_MFIS_LEN, l_iccom->name);
			if (req_iicr == NULL) {
				ret = -ENODEV;
				dev_err(l_iccom->dev, "Overlap IICR\n");
			} else {
				request_flg |= ICCOM_REQUEST_IICR;
			}
			req_eicr = request_mem_region(
					(uintptr_t)iccom_hw->mfis_eicr_addr,
					ICCOM_MFIS_LEN, l_iccom->name);
			if (req_eicr == NULL) {
				ret = -ENODEV;
				dev_err(l_iccom->dev, "Overlap EICR\n");
			} else {
				request_flg |= ICCOM_REQUEST_EICR;
			}
			req_imbr = request_mem_region(
					(uintptr_t)iccom_hw->mfis_imbr_addr,
					ICCOM_MFIS_LEN, l_iccom->name);
			if (req_imbr == NULL) {
				ret = -ENODEV;
				dev_err(l_iccom->dev, "Overlap IMBR\n");
			} else {
				request_flg |= ICCOM_REQUEST_IMBR;
			}
			req_embr = request_mem_region(
					(uintptr_t)iccom_hw->mfis_embr_addr,
					ICCOM_MFIS_LEN, l_iccom->name);
			if (req_embr == NULL) {
				ret = -ENODEV;
				dev_err(l_iccom->dev, "Overlap EMBR\n");
			} else {
				request_flg |= ICCOM_REQUEST_EMBR;
			}
			req_cta = request_mem_region(
						(uintptr_t)iccom_hw->cta_addr,
						(uintptr_t)iccom_hw->cta_size,
						l_iccom->name);
			if (req_cta == NULL) {
				ret = -ENODEV;
				dev_err(&pdev->dev, "Overlap CTA\n");
			} else {
				request_flg |= ICCOM_REQUEST_CTA;
			}
		}

		if (ret == ICCOM_OK) {
			ret = iccom_core_add(l_iccom);
		}

		if (ret == ICCOM_OK) {
			platform_set_drvdata(pdev, l_iccom);
			pr_info("ICCOM Channel%d ready\n", l_channel_no);
			pr_debug("Channel %d info\n", l_channel_no);
			pr_debug("IRQ: 0x%X", iccom_hw->irq_no);
			pr_debug("MFISARIICR: 0x%X\n",
				iccom_hw->mfis_iicr_addr);
			pr_debug("MFISAREICR: 0x%X\n",
				iccom_hw->mfis_eicr_addr);
			pr_debug("MFISARIMBR: 0x%X\n",
				iccom_hw->mfis_imbr_addr);
			pr_debug("MFISAREMBR: 0x%X\n",
				iccom_hw->mfis_embr_addr);
			pr_debug("CTA: 0x%X\n", iccom_hw->cta_addr);
			pr_debug("ACK_TIMEOUT: %llu\n", iccom_hw->ack_timeout_jf);
			pr_debug("TRG_TIMEOUT: %d\n", iccom_hw->trg_timeout);
		} else {
			if (l_iccom != NULL) {
				iccom_drv_release_iccom(l_iccom, request_flg);
				(void)iccom_core_free(l_iccom);
			}
			if (l_channel_no != ICCOM_CHANNEL_MAX) {
				pr_err("[Err]channel %d not use\n",
					l_channel_no);
			}
		}
	} else {
		/* No operation */
	}

	pr_debug("%s: End ret = %d\n", __func__, ret);

	return ret;
}

static int32_t iccom_drv_remove(struct platform_device *pdev)
{
	int32_t ret = ICCOM_OK;
	struct iccom *l_iccom;

	pr_debug("%s: Start\n", __func__);

	l_iccom = platform_get_drvdata(pdev);
	if (l_iccom != NULL) {
		iccom_drv_release_iccom(l_iccom, ICCOM_REQUEST_ALL);
		ret = iccom_core_del(l_iccom);
	}

	pr_debug("%s: End ret = %d\n", __func__, ret);

	return ret;
}

static struct platform_driver iccom_driver = {
	.probe = &iccom_drv_probe,
	.remove = __exit_p(iccom_drv_remove),
	.driver = {
		   .name = "iccom",
		   .owner = THIS_MODULE,
		   .of_match_table = iccom_drv_match,
		   },
};

static struct platform_device iccom_device = {
	.name = "iccom",
	.id = 0,
	.dev = {
			.release = &iccom_drv_device_release
		},
};

static __init int32_t iccom_drv_init(void)
{
	int32_t ret;

	pr_info("%s initialization(R-Car Rev.%s)\n",
		ICCOM_MODULE_NAME, VERSION_OF_RENESAS);

	pr_debug("%s: Start\n", __func__);

	ret = platform_driver_register(&iccom_driver);
	if (ret == 0) {
		ret = platform_device_register(&iccom_device);
		if (ret != 0) {
			platform_driver_unregister(&iccom_driver);
		}
	}

	pr_info("%s initialization finish\n", ICCOM_MODULE_NAME);

	pr_debug("%s: End\n", __func__);

	return ret;
}

static void __exit iccom_drv_exit(void)
{

	pr_info("%s de-initialization\n", ICCOM_MODULE_NAME);

	pr_debug("%s: Start\n", __func__);

	platform_device_unregister(&iccom_device);
	platform_driver_unregister(&iccom_driver);

	pr_info("%s de-initialization finish\n", ICCOM_MODULE_NAME);

	pr_debug("%s: End\n", __func__);
}

static void iccom_drv_device_release(struct device *dev)
{
	pr_debug("%s: dev=%p\n", __func__, dev);
}

static void iccom_drv_release_channel(struct iccom_channel *channel)
{
	struct iccom_communication_info *iccom_comm = NULL;

	pr_debug("%s: Start\n", __func__);

	if (channel != NULL) {
		iccom_comm = (struct iccom_communication_info *)channel->priv;
	}

	if (iccom_comm != NULL) {
		mutex_destroy(&iccom_comm->mutex_poll_intbit);
		if (iccom_comm->cta_addr != NULL) {
			iounmap(iccom_comm->cta_addr);
		}

		if (iccom_comm->mfis_embr_addr != NULL) {
			iounmap(iccom_comm->mfis_embr_addr);
		}

		if (iccom_comm->mfis_imbr_addr != NULL) {
			iounmap(iccom_comm->mfis_imbr_addr);
		}

		if (iccom_comm->mfis_eicr_addr != NULL) {
			iounmap(iccom_comm->mfis_eicr_addr);
		}

		if (iccom_comm->mfis_iicr_addr != NULL) {
			iounmap(iccom_comm->mfis_iicr_addr);
		}
		devm_kfree(channel->iccom->dev, channel);
	}

	pr_debug("%s: End\n", __func__);
}

static void iccom_drv_release_iccom(struct iccom *iccom, uint8_t request_flg)
{
	struct iccom_hardware_info *iccom_hw;

	dev_dbg(iccom->dev, "%s: Start flg = 0x%X\n", __func__, request_flg);

	iccom_hw = (struct iccom_hardware_info *)iccom->priv;
	if (iccom_hw != NULL) {
		if ((request_flg & ICCOM_REQUEST_IICR) != 0U) {
			release_mem_region((uintptr_t)iccom_hw->mfis_iicr_addr,
					ICCOM_MFIS_LEN);
		}
		if ((request_flg & ICCOM_REQUEST_EICR) != 0U) {
			release_mem_region((uintptr_t)iccom_hw->mfis_eicr_addr,
					ICCOM_MFIS_LEN);
		}
		if ((request_flg & ICCOM_REQUEST_IMBR) != 0U) {
			release_mem_region((uintptr_t)iccom_hw->mfis_imbr_addr,
					ICCOM_MFIS_LEN);
		}
		if ((request_flg & ICCOM_REQUEST_EMBR) != 0U) {
			release_mem_region((uintptr_t)iccom_hw->mfis_embr_addr,
					ICCOM_MFIS_LEN);
		}
		if ((request_flg & ICCOM_REQUEST_CTA) != 0U) {
			release_mem_region((uintptr_t)iccom_hw->cta_addr,
					(uintptr_t)iccom_hw->cta_size);
		}
		if ((request_flg & ICCOM_REQUEST_IRQ) != 0U) {
			free_irq(iccom_hw->irq_no, (void *)iccom);
		}
	}

	pr_debug("%s: End\n", __func__);
}

module_init(iccom_drv_init);
module_exit(iccom_drv_exit);

MODULE_AUTHOR("Renesas Electronics");
MODULE_DESCRIPTION(ICCOM_MODULE_NAME);
MODULE_LICENSE("GPL");
MODULE_VERSION(VERSION_OF_RENESAS);
