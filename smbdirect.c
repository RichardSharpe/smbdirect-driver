/*******************************************************************************
 * This file contains the smbdirect driver for Samba
 *
 * (c) Richard Sharpe <rsharpe@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/

#include <linux/string.h>
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "smbdirect.h"

/* Our device number, for reporting */
dev_t smbdirect_dev_no;

/*
 * A /proc file for some debugging ... replace this with configfs stuff ...
 */
static int read_proc_stuff(char *buf, char **start, off_t offset,
			int count, int *eof, void *data)
{
	int len = 0;

	len += sprintf(buf + len, " %s: Loaded with major = %u, minor = %u\n",
			"smbdirect", MAJOR(smbdirect_dev_no),
			MINOR(smbdirect_dev_no));
	*eof = 1;
	return len;
}

/*
 * The device and file ops etc
 */
struct smbd_device smbd_device;

int smbd_open(struct inode *inode, struct file *filp)
{

	return 0;
}

int smbd_release(struct inode *inode, struct file *filp)
{
	return 0;
}

long smbd_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{

	return 0;
}

struct file_operations smbd_fops = {
	.owner = THIS_MODULE,
	.open = smbd_open,
	.unlocked_ioctl = smbd_ioctl,
	.release = smbd_release,
};

static int __init smbdirect_init(void)
{
	int res = 0;

	create_proc_read_entry("driver/smbdirect", 0, NULL, 
				read_proc_stuff, NULL);

	/*
	 * Allocate a new device major number
	 */
	res = alloc_chrdev_region(&smbdirect_dev_no, 0, 1, "smbdirect");
	if (res < 0) {
		printk(KERN_ERR "Major number allocation failed\n");
		return res;
	}

	memset(&smbd_device, 0, sizeof(smbd_device));
	cdev_init(&smbd_device.cdev, &smbd_fops);
	smbd_device.cdev.owner = THIS_MODULE;

	res = cdev_add(&smbd_device.cdev, smbdirect_dev_no, 1);
	if (res) {
		printk(KERN_ERR "Unable to add smbdirect device: %d\n", res);
		goto no_cdev;
	}

no_cdev:
	return res;
}

static void __exit smbdirect_exit(void)
{
	cdev_del(&smbd_device.cdev);
	remove_proc_entry("driver/smbdirect", NULL);
}

MODULE_DESCRIPTION("smbdirect driver for Samba");
MODULE_VERSION("0.1");
MODULE_AUTHOR("rsharpe@samba.org");
MODULE_LICENSE("GPL");

module_init(smbdirect_init);
module_exit(smbdirect_exit);