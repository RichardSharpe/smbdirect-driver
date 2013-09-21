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
#include <rdma/rdma_cm.h>

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
	/*
	 * RDMA Related stuff, including our listen port.
	 */
	struct rdma_cm_id *cm_lid;
};

/*
 * Defines connections or pending connections 
 */
struct connection_struct {
	struct list_head connect_ent;
	unsigned int connected;
	unsigned long long session_id;
	/*
	 * RDMA stuff
	 */
	struct rdma_cm_id *cm_id;
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;
	/*
	 * Structures for sending and receiving RDMA stuff
	 */
	struct ib_recv_wr recv_wr; /* Initial receive ... */
	struct ib_sge recv_sgl;    /* Single SGE for now */
	struct ib_mr *recv_mr;
	struct ib_send_wr send_wr;
	struct ib_sge send_sgl;
	struct ib_mr *send_mr;
	/*
	 * Buffers ...
	 */
	char recv_buf[20];         /* an SMB Direct  Negotiate req */
	u64 recv_buf_dma;
	char send_buf[32];	   /* The SMB Direct Neg response  */
	u64 send_buf_dma;
	/*
	 * The device we are related to ...
	 */
	struct smbd_device *dev;
};

