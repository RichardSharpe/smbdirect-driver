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

#include <linux/spinlock.h>
#include <linux/cdev.h>

struct smbd_params {
	unsigned int rcv_credit_max;
	unsigned int snd_credit_max;
	unsigned int max_snd_size;
	unsigned int max_fragment_size;
	unsigned int max_rcv_size;
	unsigned int keepalive_interval;
	unsigned int sec_blob_size;
	void *sec_blob;
};

struct smbd_device {
	int initialized;
	struct smbd_params params;
	spinlock_t connection_list_lock; /* Controls connection list */
	/*
	 * List of connections or pending connections
	 */
	struct list_head connection_list;
	struct cdev cdev;
};

/*
 * Defines connections or pending connections 
 */
struct connection_struct {
	struct list_head connect_ent;
	unsigned int connected;
	unsigned long long session_id;
	struct smb_device *dev;
};

