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
#ifndef ICCOM_DRIVER_H
#define ICCOM_DRIVER_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock_types.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#include "iccom_core.h"

#define ICCOM_WAIT_TIMER_US		(1000U)	/* Unit is "us" */

#define ICCOM_CTRL_INT			(0x00000001U)
#define ICCOM_CTRL_SND_ACK		(0x00000002U)
#define ICCOM_CTRL_SND_BUFF		(0x00000004U)
#define ICCOM_CTRL_ACK_BUFF		(0x00000008U)
#define ICCOM_CTRL_INIT_END		(0x00000010U)
#define ICCOM_CTRL_FATAL		(0x00000020U)
#define ICCOM_CTRL_CLEAR_INIT_BIT	(0x000000FEU)
#define ICCOM_CTRL_READ_MFIS		(0x0000003FU)

#define ICCOM_CANNEL_NO_LEN		(2U)

#define ICCOM_FLG_CH_OPEN		(0x00000001U)
#define ICCOM_FLG_CH_INIT_END		(0x00000002U)

#define ICCOM_CTA_SBUF_NUM		(2)
#define ICCOM_CTA_RBUF_NUM		(2U)
#define ICCOM_MFIS_LEN			(4U)

#define ICCOM_SHIFT_GET_ACK_AREA	(3U)
#define ICCOM_SHIFT_GET_RECV_AREA	(2U)
#define ICCOM_SHIFT_SET_ACK_AREA	(3U)
#define ICCOM_SHIFT_SET_SEND_AREA	(2U)

#define ICCOM_REQUEST_IICR		(0x01U)
#define ICCOM_REQUEST_EICR		(0x02U)
#define ICCOM_REQUEST_IMBR		(0x04U)
#define ICCOM_REQUEST_EMBR		(0x08U)
#define ICCOM_REQUEST_CTA		(0x10U)
#define ICCOM_REQUEST_IRQ		(0x20U)
#define ICCOM_REQUEST_ALL		(0x3FU)

#define ICCOM_REG_OFFSET_LEN		(16)
#define ICCOM_CTA_MEMORY_LEN		(16)

#define ICCOM_MODULE_NAME		"ICCOM driver"

enum iccom_cta_area {
	ICCOM_CTA_UPPER = 0U,
	ICCOM_CTA_BOTTOM,

	ICCOM_CTA_COUNT
};

enum iccom_drv_write_status {
	ICCOM_WRITE_STATE_NO_OPEN = 0U,
	ICCOM_WRITE_STATE_WAIT_WRITE,
	ICCOM_WRITE_STATE_RECV_ACK,

	ICCOM_WRITE_STATE_COUNT
};

enum iccom_drv_read_status {
	ICCOM_READ_STATE_NO_OPEN = 0U,
	ICCOM_READ_STATE_WAIT_READ,
	ICCOM_READ_STATE_RECEIVING,
	ICCOM_READ_STATE_SEND_ACK,

	ICCOM_READ_STATE_COUNT
};

struct iccom_communication_info {
	uint8_t ack_arrive[ICCOM_CTA_COUNT];
	uint8_t data_arrive;
	enum iccom_drv_write_status write_status[ICCOM_CTA_COUNT];
	enum iccom_drv_read_status read_status;
	uint8_t cta_flg[ICCOM_CTA_COUNT];
	wait_queue_head_t write_q;
	wait_queue_head_t read_q;
	wait_queue_head_t ioctl_q;
	uint32_t recv_eicr[ICCOM_CTA_COUNT];
	uint32_t recv_embr[ICCOM_CTA_COUNT];
	uint32_t ack_embr[ICCOM_CTA_COUNT];
	enum iccom_cta_area recv_index[ICCOM_CTA_COUNT];
	struct spinlock spinlock_mfis_info;
	struct spinlock spinlock_cta_flg;
	struct mutex mutex_poll_intbit;
	struct semaphore sem_send_cta;
	uint8_t close_flg;
	uint32_t *mfis_iicr_addr;
	uint32_t *mfis_eicr_addr;
	uint32_t *mfis_imbr_addr;
	uint32_t *mfis_embr_addr;
	uint8_t *cta_addr;
};

struct iccom_hardware_info {
	uint32_t irq_no;
	uint32_t mfis_iicr_addr;
	uint32_t mfis_eicr_addr;
	uint32_t mfis_imbr_addr;
	uint32_t mfis_embr_addr;
	uint32_t cta_addr;
	uint32_t cta_size;
	uint64_t ack_timeout_jf;	/* convert jiffies */
	int32_t trg_timeout;
};

#endif /* ICCOM_DRIVER_H */
