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
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/miscdevice.h>

#include "iccom.h"
#include "iccom_core.h"

static LIST_HEAD(g_iccom_list_head);

static int iccom_core_open(struct inode *inode, struct file *filp);
static int iccom_core_release(struct inode *inode, struct file *filp);
static ssize_t iccom_core_read(struct file *filp, char __user *buf,
				size_t count, loff_t *pos);
static ssize_t iccom_core_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *pos);
static int32_t iccom_core_device_match(struct device *device,
				const void *devname);
static long iccom_core_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg);


/* Receive open system call */
static int iccom_core_open(struct inode *inode, struct file *filp)
{
	int32_t ret;
	struct iccom *l_iccom;
	struct iccom_channel *l_channel = NULL;

	pr_debug("%s: Start\n", __func__);

	l_iccom = container_of(filp->private_data, struct iccom, miscdev);

	ret = l_iccom->ops->open(l_iccom, &l_channel);
	if (ret == 0) {
		filp->private_data = l_channel;
	}

	pr_debug("%s: End ret = %d\n", __func__, ret);

	return ret;
}

/* Receive close system call */
static int iccom_core_release(struct inode *inode, struct file *filp)
{
	struct iccom_channel *l_channel;
	struct iccom *l_iccom;
	int32_t ret;

	pr_debug("%s: Start\n", __func__);

	l_channel = (struct iccom_channel *)filp->private_data;
	l_iccom = l_channel->iccom;

	ret = l_iccom->ops->close(l_channel);
	if (ret == 0) {
		filp->private_data = l_iccom;
	}

	pr_debug("%s: End ret = %d\n", __func__, ret);

	return ret;
}

/* Receive read system call */
static ssize_t iccom_core_read(struct file *filp, char __user *buf,
				size_t count, loff_t *pos)
{
	ssize_t ret;
	struct iccom_channel *l_channel;
	struct iccom *l_iccom;
	struct iccom_cmd cmd;

	pr_debug("%s: Start\n", __func__);

	l_channel = (struct iccom_channel *)filp->private_data;
	l_iccom = l_channel->iccom;

	cmd.buf = buf;
	cmd.count = count;

	ret = l_iccom->ops->read(l_channel, &cmd);

	pr_debug("%s: End ret = %zd\n", __func__, ret);

	return ret;
}

/* Receive write system call */
static ssize_t iccom_core_write(struct file *filp, const char __user *buf,
				size_t count, loff_t *pos)
{
	ssize_t ret;
	struct iccom_cmd cmd;
	struct iccom *l_iccom;
	struct iccom_channel *l_channel;

	pr_debug("%s: Start\n", __func__);

	l_channel = (struct iccom_channel *)filp->private_data;
	l_iccom = l_channel->iccom;

	cmd.buf = (uint8_t *)buf;
	cmd.count = count;

	ret = l_iccom->ops->write(l_channel, &cmd);

	pr_debug("%s: End ret = %zd\n", __func__, ret);

	return ret;
}

const struct file_operations iccom_fops = {
	.owner = THIS_MODULE,
	.read = &iccom_core_read,
	.write = &iccom_core_write,
	.open = &iccom_core_open,
	.release = &iccom_core_release,
	.unlocked_ioctl = &iccom_core_ioctl,
};

/* call from kernel space */
struct iccom *iccom_core_alloc(struct device *dev, int32_t channel_no,
				const struct iccom_ops *ops, size_t len)
{
	struct iccom *l_iccom;
	int32_t ret = ICCOM_OK;

	pr_debug("%s: Start\n", __func__);

	if ((dev == NULL) || (ops == NULL) ||
	    (ops->open == NULL) || (ops->close  == NULL) ||
	    (ops->read == NULL) || (ops->write == NULL)) {
		pr_err("[Err]Param err dev = %p, ops = %p\n", dev, ops);
		l_iccom = NULL;
		ret = ICCOM_NG;
	}

	if (ret == ICCOM_OK) {
		l_iccom = (struct iccom *)devm_kzalloc(
						dev,
						sizeof(struct iccom) + len,
						GFP_KERNEL);
		if (l_iccom == NULL) {
			dev_err(dev, "[Err]No memory\n");
			ret = ICCOM_NG;
		}
	}

	if (ret == ICCOM_OK) {
		l_iccom->ops = ops;
		l_iccom->dev = dev;
		l_iccom->priv = &l_iccom[1];
		l_iccom->flg = 0U;

		(void)snprintf(l_iccom->name, sizeof(l_iccom->name), "iccom%d",
				channel_no);
		mutex_init(&l_iccom->mutex_channel_open);

		l_iccom->miscdev.parent = dev;
		l_iccom->miscdev.minor = MISC_DYNAMIC_MINOR;
		l_iccom->miscdev.name = l_iccom->name;
		l_iccom->miscdev.fops = &iccom_fops;

		INIT_LIST_HEAD(&l_iccom->list);
	}

	pr_debug("%s: End\n", __func__);

	return l_iccom;
}

int32_t iccom_core_free(struct iccom *iccom)
{
	list_del(&iccom->list);
	mutex_destroy(&iccom->mutex_channel_open);
	devm_kfree(iccom->dev, iccom);

	return 0;
}

int32_t iccom_core_add(struct iccom *iccom)
{
	int32_t rc;

	pr_debug("%s: Start\n", __func__);

	if (iccom != NULL) {
		rc = misc_register(&iccom->miscdev);
		if (rc == 0) {
			dev_set_drvdata(iccom->miscdev.this_device, iccom);
			list_add_tail(&iccom->list, &g_iccom_list_head);
		} else {
			pr_err("[Err]misc_register failed %d\n", rc);
		}
	} else {
		rc = -EINVAL;
		pr_err("[Err]iccom NULL\n");
	}

	pr_debug("%s: End rc=%d\n", __func__, rc);

	return rc;
}

int32_t iccom_core_del(struct iccom *iccom)
{
	int32_t ret;

	pr_debug("%s: Start\n", __func__);
	misc_deregister(&iccom->miscdev);
	ret = iccom_core_free(iccom);
	pr_debug("%s: End\n", __func__);

	return ret;
}

static int32_t iccom_core_device_match(struct device *device,
					const void *devname)
{
	struct iccom *l_iccom;
	int32_t ret;
	int32_t ret_cmp;

	l_iccom = dev_get_drvdata(device);
	ret_cmp = strncmp(devname, l_iccom->name, sizeof(l_iccom->name));

	if (ret_cmp == 0) {
		ret = 1;
	} else {
		ret = 0;
	}

	return ret;
}

struct iccom *iccom_core_get_iccom(const int8_t *devname)
{
	struct iccom *l_iccom;
	struct list_head *l_list;
	struct device *l_device = NULL;

	pr_debug("%s: Start\n", __func__);

	list_for_each(l_list, &g_iccom_list_head) {
		l_iccom = list_entry(l_list, struct iccom, list);
		if (l_iccom != NULL) {
			l_device = class_find_device(
				l_iccom->miscdev.this_device->class,
				NULL, devname, &iccom_core_device_match);
			if (l_device != NULL) {
				break;
			}
		}
	}

	if (l_device == NULL) {
		l_iccom = NULL;
	} else {
		l_iccom = dev_get_drvdata(l_device);
	}

	pr_debug("%s: End\n", __func__);

	return l_iccom;
}

static long iccom_core_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	int64_t ret;
	struct iccom *l_iccom;
	struct iccom_channel *l_channel;

	pr_debug("%s: Start\n", __func__);

	l_channel = (struct iccom_channel *)filp->private_data;
	l_iccom = l_channel->iccom;

	ret = l_iccom->ops->ioctl(l_channel, cmd, (void *)arg);

	pr_debug("%s: End ret = %lld\n", __func__, ret);

	return ret;
}
