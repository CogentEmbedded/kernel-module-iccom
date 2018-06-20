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
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include "iccom.h"
#include "iccom_core.h"
#include "iccom_kernel_api.h"

/*****************************************************************************/
/* internal function prototype definition                                    */
/*****************************************************************************/
/* data receive thread  */
static int32_t iccom_kapi_recv_thread(void *arg);

/* channel handle check function */
static int32_t iccom_kapi_check_handle(const struct
	iccom_kapi_channel_info_t *channel_info,
	uint32_t *channel_no);

#ifdef DEBUG
/* channel handle information log function */
static void iccom_kapi_handle_log(const int8_t *func_name,
	int32_t func_line,
	struct iccom_kapi_channel_info_t *channel_info,
	uint32_t channel_no);
#endif

/*****************************************************************************/
/* "iccom kernel api" global information                                     */
/*****************************************************************************/
/* each channel information */
static struct iccom_kapi_channel_global_t
		g_kapi_channel_global[ICCOM_CHANNEL_MAX] = {NULL};
/* global mutex information */
static DEFINE_MUTEX(g_kapi_mutex_global);

/*****************************************************************************/
/*                                                                           */
/*  Name     : Iccom_lib_Init                                                */
/*  Function : Execute initialization processing of channel communicate.     */
/*             1. Open the channel of Linux ICCOM driver.                    */
/*             2. Create the receive thread.                                 */
/*             3. Create channel handle.                                     */
/*  Callinq seq.                                                             */
/*           Iccom_lib_Init(const Iccom_init_param *pIccomInit,              */
/*                          Iccom_channel_t	   *pChannelHandle)          */
/*  Input    : *pIccomInit     : Channel initialization parameter pointer.   */
/*  Output   : *pChannelHandle : Channel handle pointer.                     */
/*  Return   : 1. ICCOM_OK           (0)  : Normal                           */
/*             2. ICCOM_ERR_PARAM    (-2) : Parameter error                  */
/*             3. ICCOM_ERR_BUSY     (-5) : Channel busy                     */
/*             4. ICCOM_ERR_TO_INIT  (-6) : Initialization error             */
/*             5. ICCOM_ERR_UNSUPPORT(-8) : Unsupported channel              */
/*             6. ICCOM_NG           (-1) : Other error                      */
/*  Caller   : Device Driver                                                 */
/*                                                                           */
/*****************************************************************************/
int32_t Iccom_lib_Init(const Iccom_init_param *pIccomInit,
			Iccom_channel_t *pChannelHandle)
{
	struct iccom_kapi_channel_info_t *l_channel_info = NULL;
	struct iccom_kapi_channel_global_t *channel_global; /* ch. pointer   */
	struct iccom_channel *l_iccom_channel = NULL; /* iccom structure ptr.*/
	struct iccom *l_iccom = NULL;	/* iccom channel structure pointer   */
	int32_t retcode = ICCOM_OK;		/* return code               */
	int32_t ret;				/* call function return code */
	uint32_t l_channel_no ;			/* channel number            */
	int8_t devname[ICCOM_KDEVFILE_LEN] = {0}; /* device file name area   */
	uint8_t  mutexflg = ICCOM_OFF;		/* mutex initialized flag    */

	KAPIPRT_DBG("start : pIccomInit   = %p", (const void *)pIccomInit);

	/* check parameter pointer */
	if (pIccomInit == NULL) {
		KAPIPRT_ERR("parameter none");
		retcode = ICCOM_ERR_PARAM;
	}

	if (retcode == ICCOM_OK) {
		KAPIPRT_DBG("channel_no = %d",
			(int32_t)pIccomInit->channel_no);
		KAPIPRT_DBG("recv_buf   = %p", (void *)pIccomInit->recv_buf);
		KAPIPRT_DBG("recv_cb    = %p", (void *)pIccomInit->recv_cb);
		KAPIPRT_DBG("recv_thread = %p", (void *)iccom_kapi_recv_thread);

		l_channel_no = (uint32_t)pIccomInit->channel_no;
		/* check initialization parameter contents */
		if ((pIccomInit->recv_buf == NULL) ||
		    (pIccomInit->recv_cb == NULL) ||
		    (l_channel_no >= (uint32_t)ICCOM_CHANNEL_MAX)) {
			KAPIPRT_ERR(
				"parameter err : recv_buf = %p, recv_cb = %p,"
				" channel No. = %d",
				(void *)pIccomInit->recv_buf,
				(void *)pIccomInit->recv_cb, l_channel_no);
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		/* create device file name */
		KAPIPRT_DBG("snprintf para : 2nd = %u, 4th = %s, 5th = %u",
			ICCOM_KDEVFILE_LEN, ICCOM_KDEVFILENAME, l_channel_no);
		ret = snprintf((char *)devname, ICCOM_KDEVFILE_LEN,
				"%s%d", ICCOM_KDEVFILENAME, l_channel_no);
		if (ret < 0) {
			KAPIPRT_ERR(
				"cannot create device file name : err = %d",
				ret);
			retcode = ICCOM_NG;
		}
	}

	if (retcode == ICCOM_OK) {
		KAPIPRT_DBG("devname = %s", devname);
		/* get iccom_structure pointer */
		l_iccom = iccom_core_get_iccom(devname);
		KAPIPRT_DBG("l_iccom =%p", l_iccom);
		/* iccom structure pointer none */
		if (l_iccom == NULL) {
			KAPIPRT_ERR(
				"channel unsupport : channel No. = %d,"
				" device file name = %s",
				l_channel_no, devname);
			retcode = ICCOM_ERR_UNSUPPORT;
		}
	}

	if (retcode == ICCOM_OK) {
		/* open channel */
		KAPIPRT_DBG("open para : iccom = %p, &iccom_channel= %p",
				l_iccom, &l_iccom_channel);
		ret = l_iccom->ops->open(l_iccom, &l_iccom_channel);
		KAPIPRT_DBG("open channel:retcode = %x, l_iccom_channel = %p",
			    ret, l_iccom_channel);
		if (ret != ICCOM_OK) {
			switch (ret) {
			case -EBUSY:
				retcode = ICCOM_ERR_BUSY;
				break;
			case -EDEADLK:
				retcode = ICCOM_ERR_TO_INIT;
				break;
			default:
				retcode = ICCOM_NG;
				break;
			}
			KAPIPRT_ERR("open err : channel No. = %d, error = %d,"
				" return code = %d",
				l_channel_no, ret, retcode);
		}
	}

	if (retcode == ICCOM_OK) {
		if (l_iccom_channel == NULL) {
			/* iccom channel structure pointer none */
			/* illegal case of iccom driver */
			KAPIPRT_ERR(
			  "iccom channel information none : channel No. = %d",
			   l_channel_no);
			retcode = ICCOM_NG;
		}
	}

	if (retcode == ICCOM_OK) {
		/* get channel handle information area */
		KAPIPRT_DBG("kzalloc para : 1st = %lu, 2nd = %d",
				sizeof(*l_channel_info), (int32_t)GFP_KERNEL);
		l_channel_info = (struct iccom_kapi_channel_info_t *)
				   kzalloc(sizeof(*l_channel_info), GFP_KERNEL);
		if (l_channel_info == NULL) {
			KAPIPRT_ERR("not get channel handle area");
			retcode = ICCOM_NG;
		}
		KAPIPRT_DBG("l_channel_info = %p", (void *)l_channel_info);
	}

	if (retcode == ICCOM_OK) {
		/* initial setting channel handle information */
		l_channel_info->channel_no = pIccomInit->channel_no;
		l_channel_info->send_req_cnt = 0U;
		l_channel_info->recv_buf = pIccomInit->recv_buf;
		l_channel_info->recv_cb = pIccomInit->recv_cb;
		l_channel_info->channel = l_iccom_channel;

		channel_global = &g_kapi_channel_global[l_channel_no];

		/* initialize completion */
		KAPIPRT_DBG("init_completion para = %p",
			    (void *)&l_channel_info->recv_end);
		init_completion(&l_channel_info->recv_end);

		/* initialize mutex information */
		KAPIPRT_DBG("mutex_init para = %p",
				(void *)&channel_global->mutex_channel_info);
		mutex_init(&channel_global->mutex_channel_info);
		mutexflg = ICCOM_ON;

		/* create & run data receive thread */
		KAPIPRT_DBG("kthread_run para : 1st = %p, 2nd = %p, 4th = %u",
				(void *)iccom_kapi_recv_thread,
				(void *)l_channel_info, l_channel_no);
		l_channel_info->recv_thread =
			kthread_run(iccom_kapi_recv_thread, l_channel_info,
			"Iccom_kapi_recv_thread ch = %d", l_channel_no);
		KAPIPRT_DBG("l_channel_info->recv_thread = %p",
			    (void *)l_channel_info->recv_thread);
		if (IS_ERR(l_channel_info->recv_thread)) {
			KAPIPRT_ERR("receive thread creation err : err = %p",
				(void *)l_channel_info->recv_thread);
			retcode = ICCOM_NG;
		}
	}

	if (retcode == ICCOM_OK) {
		/* lock global mutex */
		KAPIPRT_DBG("mutex_lock para = %p",
				(void *)(&g_kapi_mutex_global));
		mutex_lock(&g_kapi_mutex_global);
		/* set channel handle pointer */
		channel_global->channel_info = l_channel_info;
		/* unlock global mutex */
		KAPIPRT_DBG("mutex_unlock para : 1st = %p",
				(void *)(&g_kapi_mutex_global));
		mutex_unlock(&g_kapi_mutex_global);

		/* set channel handle for Device Driver */
		*pChannelHandle = (Iccom_channel_t)l_channel_info;

		/* output channel handle debug log */
		KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info, l_channel_no);

	} else {
		/* abnormal correspondence */
		/* channel opened already */
		if (l_iccom_channel != NULL) {
			KAPIPRT_DBG("close para = %p",
				(void *)l_iccom_channel);
			l_iccom->ops->close(l_iccom_channel);
		}
		/* channel mutex initialized already */
		if (mutexflg == ICCOM_ON) {
			KAPIPRT_DBG("mutex_destroy para = %p",
				(void *)&channel_global->mutex_channel_info);
			mutex_destroy(&channel_global->mutex_channel_info);
		}
		/* memory allocated already */
		if (l_channel_info != NULL) {
			KAPIPRT_DBG("kfree para = %p", (void *)l_channel_info);
			kfree(l_channel_info);
		}
	}
	KAPIPRT_DBG("end : retcode = %d", retcode);
	return retcode;
}
EXPORT_SYMBOL(Iccom_lib_Init);

/*****************************************************************************/
/*                                                                           */
/*  Name     : Iccom_lib_Send                                                */
/*  Function : Send data from Linux side to CR7 side.                        */
/*  Callinq seq.                                                             */
/*           Iccom_lib_Send(const Iccom_send_param *pIccomSend)              */
/*  Input    : * pIccomSend : The send parameter pointer.                    */
/*  Return   : 1. ICCOM_OK           (0)  : Normal                           */
/*             2. ICCOM_ERR_PARAM    (-2) : Parameter error                  */
/*             3. ICCOM_ERR_BUF_FULL (-3) : Buffer full error                */
/*             4. ICCOM_ERR_TO_ACK   (-4) : Acknowledgement timeout erorr    */
/*             5. ICCOM_ERR_TO_SEND  (-7) : Data send timeout error          */
/*             6: ICCOM_ERR_SIZE     (-9) : Send size illegal                */
/*             7. ICCOM_NG           (-1) : Other error                      */
/*  Caller   : Device Driver                                                 */
/*  Note     : Use of channel number in this function is necessary to use    */
/*             value obtained in call of iccom_lib_check_handle function.    */
/*                                                                           */
/*****************************************************************************/
int32_t Iccom_lib_Send(const Iccom_send_param *pIccomSend)
{
	struct iccom_kapi_channel_info_t *l_channel_info; /* ch handle info. */
	struct iccom_kapi_channel_global_t *channel_global; /* global pointer*/
	struct iccom_cmd cmd;			/* transmission info         */
	ssize_t write_count;			/* send size(result)         */
	int32_t retcode = ICCOM_OK;		/* return code               */
	int32_t ret;				/* call function return code */
	uint32_t l_channel_no;			/* channel number            */
	uint32_t req_update_flag = ICCOM_OFF;   /* req. counter update flag  */

	KAPIPRT_DBG("start : pIccomSend = %p", (const void *)pIccomSend);

	/* check parameter pointer */
	if (pIccomSend == NULL) {
		KAPIPRT_ERR("parameter none");
		retcode = ICCOM_ERR_PARAM;
	}

	if (retcode == ICCOM_OK) {
		KAPIPRT_DBG("send_size  = %d", pIccomSend->send_size);
		KAPIPRT_DBG("send_buf   = %p", (void *)pIccomSend->send_buf);

		/* check send parameter contents */
		if ((pIccomSend->send_size > ICCOM_BUF_MAX_SIZE) ||
		    (pIccomSend->send_buf == NULL)) {
			KAPIPRT_ERR(
				"parameter err : send_size = %u,"
				" send_buf = %p",
				pIccomSend->send_size,
				(void *)pIccomSend->send_buf);
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		l_channel_info =
			(struct iccom_kapi_channel_info_t *)
				pIccomSend->channel_handle;
		/* check channel handle & get channel number */
		KAPIPRT_DBG("iccom_kapi_check_handle para  1st = %p, 2nd = %p",
				(void *)l_channel_info, (void *)&l_channel_no);
		ret = iccom_kapi_check_handle(l_channel_info, &l_channel_no);
		KAPIPRT_DBG("iccom_kapi_check_handle ret=%d, l_channel_no=%u",
			    ret, l_channel_no);
		if (ret != ICCOM_OK) {
			KAPIPRT_ERR("channel handle err : err = %d", ret);
			retcode = ret;
		}
	}

	if (retcode == ICCOM_OK) {
		channel_global = &g_kapi_channel_global[l_channel_no];

		/* lock channel handle */
		KAPIPRT_DBG("mutex_lock para = %p",
				(void *)&channel_global->mutex_channel_info);
		mutex_lock(&channel_global->mutex_channel_info);

		/* output channel handle debug log */
		KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info, l_channel_no);

		/* check channel handle pointer of global */
		if (channel_global->channel_info == NULL) {
			KAPIPRT_ERR("channel not open : channel No. = %u",
				l_channel_no);
			KAPIPRT_DBG("mutex_unlock para = %p",
				(void *)&channel_global->mutex_channel_info);
			mutex_unlock(&channel_global->mutex_channel_info);
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		/* increment send request counter */
		l_channel_info->send_req_cnt++;
		req_update_flag = ICCOM_ON;

		/* unlock channel handle */
		KAPIPRT_DBG("mutex_unlock para = %p",
				(void *)&channel_global->mutex_channel_info);
		mutex_unlock(&channel_global->mutex_channel_info);

		/* set send information */
		cmd.buf = pIccomSend->send_buf;		/* send budder       */
		cmd.count = pIccomSend->send_size;	/* send size(request)*/

		/* send data */
		KAPIPRT_DBG("write function para : 1st = %p, 2nd = %p"
			    " cmd.buf = %p, cmd.count = %lu",
			    (void *)l_channel_info->channel, (void *)&cmd,
			    (void *)cmd.buf, cmd.count);
		write_count = l_channel_info->channel->iccom->ops->write(
			l_channel_info->channel, &cmd);
		KAPIPRT_DBG("send data : send size(result) = %ld",
							write_count);
		/* output channel handle debug log */
		KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info, l_channel_no);
		if (write_count != (ssize_t)pIccomSend->send_size) {
			if (write_count < 0) {
				switch (write_count) {
				case -ENOSPC:
					retcode = ICCOM_ERR_BUF_FULL;
					break;
				case -ETIMEDOUT:
					retcode = ICCOM_ERR_TO_ACK;
					break;
				case -EDEADLK:
					retcode = ICCOM_ERR_TO_SEND;
					break;
				default:
					retcode = ICCOM_NG;
					break;
				}
				KAPIPRT_ERR(
					"send err : channel No. = %d,"
					" error  = %d, return ocde = %d",
					l_channel_no, (int32_t)write_count,
					retcode);
			}
			/* illegal send size */
			else {
				KAPIPRT_ERR(
					"send size mismatch : channel No. = %d,"
					" request size = %u, result size = %ld",
					l_channel_no, pIccomSend->send_size,
					write_count);
				retcode = ICCOM_ERR_SIZE;
			}
		}
	}

	/* check send request counter increment */
	if (req_update_flag == ICCOM_ON) {
		KAPIPRT_DBG("send count decrement");

		/* lock channel handle */
		KAPIPRT_DBG("mutex_lock para = %p",
			(void *)&channel_global->mutex_channel_info);
		mutex_lock(&channel_global->mutex_channel_info);
		/* decrement send request counter */
		l_channel_info->send_req_cnt--;
		/* unlock channel handle */
		KAPIPRT_DBG("mutex_unlock para = %p",
			(void *)&channel_global->mutex_channel_info);
		mutex_unlock(&channel_global->mutex_channel_info);
		/* output channel handle debug log */
		KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info, l_channel_no);
	}

	KAPIPRT_DBG("end : retcode = %d", retcode);
	return retcode;
}
EXPORT_SYMBOL(Iccom_lib_Send);

/*****************************************************************************/
/*                                                                           */
/*  Name     : Iccom_lib_Final                                               */
/*  Function : Execute finalization processing of channel communication.     */
/*             1. End the receive thread.                                    */
/*             2. Close the channel of Linux ICCOM driver.                   */
/*             3. Release channel handle.                                    */
/*  Callinq seq.                                                             */
/*           Iccom_lib_Final(Iccom_channel_t ChannelHandle)                  */
/*  Input    : *ChannelHandle  : Channel handle                              */
/*  Return   : 1. ICCOM_OK           (0)  : Normal                           */
/*             2. ICCOM_ERR_PARAM    (-2) : Parameter error                  */
/*             3. ICCOM_NG           (-1) : 1 Data sending                   */
/*                                        : 2 data receiving                 */
/*                                          3 other                          */
/*  Caller   : Device Driver                                                 */
/*  Note     : Use of channel number in this function is necessary to use    */
/*             value obtained in call of iccom_lib_check_handle function.    */
/*                                                                           */
/*****************************************************************************/
int32_t Iccom_lib_Final(Iccom_channel_t ChannelHandle)
{
	struct iccom_kapi_channel_info_t *l_channel_info; /* ch handle info. */
	struct iccom_kapi_channel_global_t *channel_global; /* global pointer*/
	int32_t retcode = ICCOM_OK;		/* return code               */
	int32_t ret;				/* call function return code */
	uint32_t l_channel_no;			/* channel number            */
	uint8_t channel_mutex_flag = ICCOM_OFF; /* channel mutex flag        */

	KAPIPRT_DBG("start : ChannelHandle = %p", (void *)ChannelHandle);

	l_channel_info = (struct iccom_kapi_channel_info_t *)ChannelHandle;
	/* check channel handle & get channel number */
	KAPIPRT_DBG("iccom_kapi_check_handle para  1st = %p, 2nd = %p",
		    (void *)l_channel_info, (void *)&l_channel_no);
	ret = iccom_kapi_check_handle(l_channel_info, &l_channel_no);
	KAPIPRT_DBG("iccom_kapi_check_handle ret = %d ,l_channel_no = %u",
		    ret, l_channel_no);
	if (ret != ICCOM_OK) {
		KAPIPRT_ERR("channel handle err : err = %d", ret);
		retcode = ret;
	}

	if (retcode == ICCOM_OK) {
		channel_global = &g_kapi_channel_global[l_channel_no];

		/* lock channel handle */
		KAPIPRT_DBG("mutex_lock para = %p",
			(void *)&channel_global->mutex_channel_info);
		mutex_lock(&channel_global->mutex_channel_info);
		channel_mutex_flag = ICCOM_ON;

		/* output channel handle debug log */
		KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info, l_channel_no);

		/* check channel handle pointer of global */
		if (channel_global->channel_info == NULL) {
			KAPIPRT_ERR("channel not open : channel No. = %u",
				l_channel_no);
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		/*  check data sending now */
		if (l_channel_info->send_req_cnt != 0U) {
			KAPIPRT_ERR(
				"data sending : channel No. = %u,"
				" send request count = %u\n",
				l_channel_no, l_channel_info->send_req_cnt);
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		/* cancel receive */
		KAPIPRT_DBG("ioctl function para 1st = %p, 2nd = %d",
			(void *)l_channel_info->channel,
			ICCOM_IOC_CANCEL_RECEIVE);
		ret = l_channel_info->channel->iccom->ops->ioctl(
			l_channel_info->channel,
			ICCOM_IOC_CANCEL_RECEIVE, NULL);
		KAPIPRT_DBG("ioctl : retcode = %x", ret);
		if (ret != ICCOM_OK) {
			retcode = ICCOM_NG;
			KAPIPRT_ERR("ioctl : channel No. = %d,"
			" error  = %d, return code = %d",
				   l_channel_no, ret, retcode);
		}
	}

	if (retcode == ICCOM_OK) {
		/* wait end of the receive thread */
		KAPIPRT_DBG("wait_for_completion_interruptible para %p",
			(void *)&l_channel_info->recv_end);
		(void)wait_for_completion_interruptible(
			&l_channel_info->recv_end);

		/* close channel */
		KAPIPRT_DBG("close function para %p",
			(void *)l_channel_info->channel);
		ret = l_channel_info->channel->iccom->ops->close(
			l_channel_info->channel);
		KAPIPRT_DBG("close channel : retcode = %x", ret);

		/* lock global mutex */
		KAPIPRT_DBG("mutex_lock para = %p",
			(void *)&g_kapi_mutex_global);
		mutex_lock(&g_kapi_mutex_global);

		/* clear channel handle pointer */
		channel_global->channel_info = NULL;

		/* output channel handle debug log */
		KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info, l_channel_no);

		/* free channel handle */
		KAPIPRT_DBG("kfree = %p", (void *)l_channel_info);
		kfree(l_channel_info);

		/* unlock global mutex */
		KAPIPRT_DBG("mutex_unlock para = %p",
			(void *)&g_kapi_mutex_global);
		mutex_unlock(&g_kapi_mutex_global);

		/* unlock channel handle & delete mutex */
		KAPIPRT_DBG("mutex_unlock para = %p",
			(void *)&channel_global->mutex_channel_info);
		mutex_unlock(&channel_global->mutex_channel_info);
		KAPIPRT_DBG("mutex_destroy para = %p",
			(void *)&channel_global->mutex_channel_info);
		mutex_destroy(&channel_global->mutex_channel_info);

	} else {
		if (channel_mutex_flag == ICCOM_ON) {
			KAPIPRT_DBG("mutex_unlock para = %p",
				(void *)&channel_global->mutex_channel_info);
			mutex_unlock(&channel_global->mutex_channel_info);
		}
	}

	KAPIPRT_DBG("end : retcode = %d", retcode);
	return retcode;
}
EXPORT_SYMBOL(Iccom_lib_Final);

/*****************************************************************************/
/*                                                                           */
/*  Name     : iccom_kapi_recv_thread                                        */
/*  Function : 1. Receive data from CR7 side.                                */
/*             2. Call callback function for pass the received data.         */
/*  Callinq seq.                                                             */
/*           iccom_kapi_recv_thread(void *arg)                               */
/*  Input    : *arg            : Channel handle information                  */
/*  Return   : ICCOM_OK                                                      */
/*  Note     : This thread is created in execution of kthread_run function   */
/*             in iccom_lib_init. In addition, end with setting of           */
/*             recv_thread_end of iccom_lib_final.                           */
/*                                                                           */
/*****************************************************************************/
static int32_t iccom_kapi_recv_thread(void *arg)
{
	struct iccom_kapi_channel_info_t *l_channel_info; /* ch handle info. */
	struct iccom_cmd cmd;			/* transmission info         */
	ssize_t read_size;			/* receive size(result)      */

	KAPIPRT_DBG("start arg = %p", (void *)arg);
	l_channel_info = (struct iccom_kapi_channel_info_t *)arg;
	KAPIPRT_DBG("l_channel_info = %p", (void *)l_channel_info);
	KAPIPRT_DBG("callback= %p , channel No.= %d , recv_buf = %p r_end = %p",
		(void *)l_channel_info->recv_cb,
		(int32_t)l_channel_info->channel_no,
		(void *)l_channel_info->recv_buf,
		(void *)&l_channel_info->recv_end);

	/* loop until setting the recv_thread_end */
	while (1) {
		/* set receive information */
		cmd.buf   = l_channel_info->recv_buf;	/* receive buffer    */
		cmd.count = (size_t)ICCOM_BUF_MAX_SIZE;	/* receive max size  */
		/* receive data */
		KAPIPRT_DBG(
			"read function para : 1st = %p, 2nd = %p"
			" cmd.buf = %p, cmd.count = %lu",
			(void *)l_channel_info->channel, (void *)&cmd,
			cmd.buf, cmd.count);
		read_size = l_channel_info->channel->iccom->ops->read(
			l_channel_info->channel, &cmd);
		KAPIPRT_DBG("receive data : receive size = %ld", read_size);
		if (read_size >= 0) {
			KAPIPRT_DBG(
				"call callback function : call back = %p"
				" channel No. = %d, size = %ld, buf = %p",
				(void *)l_channel_info->recv_cb,
				(int32_t)l_channel_info->channel_no,
				read_size, (void *)l_channel_info->recv_buf);
			/* call callback function */
			(*l_channel_info->recv_cb)(
				l_channel_info->channel_no,
				(uint32_t)read_size,
				l_channel_info->recv_buf);
		} else {
			/* end data receive */
			if (read_size == -ECANCELED) {
				break;
			}
			KAPIPRT_ERR(
				"receive err : channel No. = %d, retcode = %ld",
				(int32_t)l_channel_info->channel_no, read_size);
		}
	}
	/* cancel the wait of Iccom_lib_Final */
	KAPIPRT_DBG("complete para %p", (void *)&l_channel_info->recv_end);
	complete(&l_channel_info->recv_end);

	/* output channel handle debug log */
	KAPI_CANANEL_HANDLE_DBGLOG(l_channel_info,
		(int32_t)l_channel_info->channel_no);
	KAPIPRT_DBG("end : channel no : %d",
		(int32_t)l_channel_info->channel_no);
	return ICCOM_OK;
}

/*****************************************************************************/
/*                                                                           */
/*  Name     : iccom_kapi_check_handle                                       */
/*  Function : Check channel handle and get channel number                   */
/*  Callinq seq.                                                             */
/*           iccom_kapi_check_handle(                                        */
/*                           struct iccom_kapi_channel_info_t *channel_info, */
/*                           uint32_t                         *channel_no)   */
/*  Input    : *channel_info   : Channel handle pointer.                     */
/*  Output   : *channel_no     : Channel number pointer.                     */
/*  Return   : 1. ICCOM_OK           (0)  : Normal                           */
/*             2. ICCOM_ERR_PARAM    (-2) : Parameter error                  */
/*  Caller   : Iccom_lib_Send,Iccom_lib_Final                                */
/*                                                                           */
/*****************************************************************************/
static int32_t iccom_kapi_check_handle(const struct
				iccom_kapi_channel_info_t *channel_info,
				uint32_t *channel_no)
{
	int32_t retcode = ICCOM_OK;	/* return code                       */
	uint32_t ch_loop;		/* loop counter of channel number    */

	KAPIPRT_DBG("start channel_info = %p", (const void *)channel_info);

	/* check channel handle pointer(client side channel handle). */
	if (channel_info == NULL) {
		KAPIPRT_ERR("channel handle none");
		retcode = ICCOM_ERR_PARAM;
	}

	if (retcode == ICCOM_OK) {
		/* lock global mutex */
		KAPIPRT_DBG("mutex_lock para = %p",
			(void *)&g_kapi_mutex_global);
		mutex_lock(&g_kapi_mutex_global);

		/* search channel handle pointer */
		for (ch_loop = 0U; ch_loop < (uint32_t)ICCOM_CHANNEL_MAX;
		     ch_loop++) {
			/* check channel handle pointer */
			if (g_kapi_channel_global[ch_loop].channel_info ==
				channel_info) {
				break;
			}
		}

		/* unlock global mutex */
		KAPIPRT_DBG("mutex_unlock para = %p",
			(void *)&g_kapi_mutex_global);
		mutex_unlock(&g_kapi_mutex_global);

		/* check loop count */
		if (ch_loop >= (uint32_t)ICCOM_CHANNEL_MAX) {
			/* not found channel handle pointer */
			KAPIPRT_ERR("not found channel handle pointer");
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		/* check channel number */
		if ((uint32_t)channel_info->channel_no != ch_loop) {
			KAPIPRT_ERR(
				"mismatch channel No. : handle = %u,"
				" global = %u",
				(uint32_t)channel_info->channel_no, ch_loop);
			retcode = ICCOM_ERR_PARAM;
		}
	}

	if (retcode == ICCOM_OK) {
		/* set channel number */
		*channel_no = (uint32_t)channel_info->channel_no;
	}

	KAPIPRT_DBG("end:channel No. %d, retcode = %d, loop_connter %u",
		*channel_no, retcode, ch_loop);
	return retcode;
}

#ifdef DEBUG
/*****************************************************************************/
/*                                                                           */
/*  Name     : iccom_kapi_handle_log                                         */
/*  Function : Log channel handle information.                               */
/*  Callinq seq.                                                             */
/*             iccom_kapi_handle_log(const int8_t *func_name,                */
/*                      int8_t                    *func_line,                */
/*                      struct iccom_kapi_channel_info_t *channel_info,      */
/*                      uint32_t                channel_no)                  */
/*  Input    : *func_name  : function name pointer                           */
/*             func_line   : Line number                                     */
/*             *channel_info  : Channel handle information pointer.          */
/*             channel_no     : Channel number                               */
/*  Return   : NON                                                           */
/*  Caller  : The function in iccom_kernel_api.c                             */
/*                                                                           */
/*****************************************************************************/
static void iccom_kapi_handle_log(const int8_t *func_name,
			int32_t func_line,
			struct iccom_kapi_channel_info_t *channel_info,
			uint32_t channel_no)
{
	(void)pr_debug("%s() L%d g_channel_no = %d\n",
			func_name, func_line, channel_no);
	(void)pr_debug("g_ch[0]-[3] = %16p %16p %16p %16p\n",
		(void *)g_kapi_channel_global[0].channel_info,
		(void *)g_kapi_channel_global[1].channel_info,
		(void *)g_kapi_channel_global[2].channel_info,
		(void *)g_kapi_channel_global[3].channel_info);
	(void)pr_debug("g_ch[4]-[7] = %16p %16p %16p %16p\n",
		(void *)g_kapi_channel_global[4].channel_info,
		(void *)g_kapi_channel_global[5].channel_info,
		(void *)g_kapi_channel_global[6].channel_info,
		(void *)g_kapi_channel_global[7].channel_info);

	if (g_kapi_channel_global[channel_no].channel_info != NULL) {
		(void)pr_debug("    channel_no = %d\n",
			(int32_t)channel_info->channel_no);
		(void)pr_debug("    send_cnt   = %d\n",
			channel_info->send_req_cnt);
		(void)pr_debug("    recv_buf   = %p\n",
			(void *)channel_info->recv_buf);
		(void)pr_debug("    recv_cb    = %p\n",
			(void *)channel_info->recv_cb);
		(void)pr_debug("channel    = %p\n",
			(void *)channel_info->channel);
		(void)pr_debug("recv_end = %p\n",
			(void *)&channel_info->recv_end);
		(void)pr_debug("*recv_thread_tsk = %p\n",
			(void *)channel_info->recv_thread);
	}

	(void)pr_debug("%s() L%d mutex_channel_info = %d\n",
		func_name, func_line, channel_no);
	(void)pr_debug("g_ch_mutex[0]-[3] = %16p %16p %16p %16p\n",
		(void *)&g_kapi_channel_global[0].mutex_channel_info,
		(void *)&g_kapi_channel_global[1].mutex_channel_info,
		(void *)&g_kapi_channel_global[2].mutex_channel_info,
		(void *)&g_kapi_channel_global[3].mutex_channel_info);
	(void)pr_debug("g_ch_mutex[4]-[7] = %16p %16p %16p %16p\n",
		(void *)&g_kapi_channel_global[4].mutex_channel_info,
		(void *)&g_kapi_channel_global[5].mutex_channel_info,
		(void *)&g_kapi_channel_global[6].mutex_channel_info,
		(void *)&g_kapi_channel_global[7].mutex_channel_info);
	(void)pr_debug("%s() L%d g_lib_mutex_global = %p\n",
		func_name, func_line, (void *)&g_kapi_mutex_global);
}
#endif
