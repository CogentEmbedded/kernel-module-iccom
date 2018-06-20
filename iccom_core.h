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
#ifndef ICCOM_CORE_H
#define ICCOM_CORE_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/list.h>

#define VERSION_OF_RENESAS		"1.0.0"

#define ICCOM_ON			(1U)	/* FLAG ON  */
#define ICCOM_OFF			(0U)	/* FLAG OFF */

#define ICCOM_MAX_ICCOM_DEV_NAME	(128)

/* ioctl request command */
#define ICCOM_IOC_CANCEL_RECEIVE	(1)

struct iccom {
	struct list_head list;
	int8_t name[ICCOM_MAX_ICCOM_DEV_NAME];
	void *priv; /* set iccom_hardware_info. */
	const struct iccom_ops *ops;
	struct device *dev;
	struct miscdevice miscdev;
	uint32_t flg;
	struct iccom_channel *channel;
	struct mutex mutex_channel_open;
};

struct iccom_channel {
	struct iccom *iccom;
	void *priv; /* set iccom_communication_info. */
};

struct iccom_cmd {
	uint8_t *buf;
	size_t count;
};

struct iccom_ops {
	struct module *owner;
	const char *type;
	int32_t (*open)(struct iccom *iccom,
			struct iccom_channel **channel_out);
	int32_t (*close)(struct iccom_channel *channel);
	ssize_t (*read)(struct iccom_channel *channel, struct iccom_cmd *cmd);
	ssize_t (*write)(struct iccom_channel *channel, struct iccom_cmd *cmd);
	int32_t (*ioctl)(struct iccom_channel *channel, int32_t req, void *arg);
};

struct iccom *iccom_core_alloc(struct device *dev, int32_t channel_no,
				const struct iccom_ops *ops, size_t len);
int32_t iccom_core_free(struct iccom *iccom);
int32_t iccom_core_add(struct iccom *iccom);
int32_t iccom_core_del(struct iccom *iccom);

struct iccom *iccom_core_get_iccom(const int8_t *devname);

#endif /* ICCOM_CORE_H */
