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
#ifndef ICCOM_KERNEL_API_H
#define ICCOM_KERNEL_API_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include "iccom.h"
#include "iccom_core.h"

/*****************************************************************************/
/* define definition                                                         */
/*****************************************************************************/
#define ICCOM_KDEVFILENAME	"iccom"	  /* device file name fixed portion  */
#define ICCOM_KDEVFILE_LEN	(16U)	  /* device file name maximum length */

/*****************************************************************************/
/* structure definition                                                      */
/*****************************************************************************/
/* channel handle information */
struct  iccom_kapi_channel_info_t {
	enum Iccom_channel_number channel_no;	/* channel number            */
	uint32_t send_req_cnt;			/* send request counter      */
	uint8_t *recv_buf;			/* data receive buffer       */
	Iccom_recv_callback_t recv_cb;		/* callback function         */
	struct iccom_channel *channel;		/* iccom channel structure   */
	struct completion recv_end;		/* completion structure      */
	struct task_struct *recv_thread;	/* receive thread task str.  */
};

/* channel global information */
struct iccom_kapi_channel_global_t {
	struct iccom_kapi_channel_info_t *channel_info; /*channel handle inf.*/
	struct mutex mutex_channel_info;	/* mutex information         */
};

/*****************************************************************************/
/* LOG definition                                                            */
/*****************************************************************************/
#define KAPIPRT_ERR(fmt, ...) \
	(void)pr_err("[ERR]%s() : "fmt"\n", __func__, ## __VA_ARGS__)

#define KAPIPRT_NRL(fmt, ...) \
	(void)pr_info("[NML]%s() L%d: "fmt"\n", \
		__func__, __LINE__, ## __VA_ARGS__)
#ifdef DEBUG
#define KAPIPRT_DBG(fmt, ...) \
	(void)pr_debug("[DBG]%s() L%d: "fmt"\n", \
		__func__, __LINE__, ## __VA_ARGS__)
#else
#define KAPIPRT_DBG(fmt, ...)
#endif

#ifdef DEBUG
#define KAPI_CANANEL_HANDLE_DBGLOG(CHANNEL_INFO, CHANNEL_NO) \
		iccom_kapi_handle_log((const int8_t *)__func__, \
		__LINE__, (CHANNEL_INFO), (CHANNEL_NO))
#else
#define KAPI_CANANEL_HANDLE_DBGLOG(CHANNEL_INFO, CHANNEL_NO) (void)0
#endif

#endif /* ICCOM_KERNEL_API_H */
