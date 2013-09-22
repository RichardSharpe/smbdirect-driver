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

#include <linux/init.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/ioctl.h>
#include <linux/inet.h>
#include <uapi/linux/in.h>
#include <linux/in6.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/seq_file.h>

#include "smbdirect.h"

/*
 * TODO: Convert to using dev_dbg, but probably need a platform_device for
 * that.
 */

/*
 * The port number we listen on
 */
#define SMB_DIRECT_PORT 5445

/* Max CQ depth per queue ... */
#define MAX_CQ_DEPTH 128

/* Our device number, for reporting */
dev_t smbdirect_dev_no;

/*
 * A /proc file for some debugging ... replace this with configfs stuff ...
 */
static int read_proc_stuff(struct seq_file *m, void *data)
{

	seq_printf(m, " %s: Loaded with major = %u, minor = %u\n",
			"smbdirect",
			MAJOR(smbdirect_dev_no),
			MINOR(smbdirect_dev_no));
	return 0;
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

/*
 * Relocate these definitions to a .h file
 */
#define SMBD_IOC_VAL 's'
#define SMBD_SET_PARAMS     _IOR(SMBD_IOC_VAL, 1, void *)
#define SMBD_GET_MEM_PARAMS _IOW(SMBD_IOC_VAL, 2, void *)
#define SMBD_SET_SESSION_ID _IOR(SMBD_IOC_VAL, 3, void *)

/*
 * Get the params and mark us as initialized
 */
long handle_set_params(unsigned long arg)
{
	int res = 0;
	struct smbd_params *params = (void __user *)arg;

	res = copy_from_user(&smbd_device.params, 
			params, 
			sizeof(struct smbd_params));

	if (!res) {
		printk(KERN_ERR "Error: Memory for SMBD_SET_PARAMS "
			"not accessible\n");
		return -EFAULT;
	}

	/*
	 * Check the values and also copy the blob
	 */

	/*
	 * We are initialized now
	 */
	smbd_device.initialized = 1;

	return res;
}

long smbd_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	int res = -EINVAL;

	printk(KERN_INFO "Handling ioctl: %0X\n", cmd);

	switch (cmd) {
	case SMBD_SET_PARAMS:
		return handle_set_params(arg);
		break;

	case SMBD_GET_MEM_PARAMS:

		break;

	case SMBD_SET_SESSION_ID:

		break;

	default:

		break;
	}

	return res;
}

struct file_operations smbd_fops = {
	.owner = THIS_MODULE,
	.open = smbd_open,
	.unlocked_ioctl = smbd_ioctl,
	.release = smbd_release,
};

/*
 * Clean up buffers etc ...
 */
static void
smbd_free_buffers(struct connection_struct *conn)
{
	if (conn->recv_mr)
		ib_dereg_mr(conn->recv_mr);
	ib_dma_unmap_single(conn->cm_id->device,
			    conn->recv_buf_dma,
			    sizeof(conn->recv_buf),
			    DMA_FROM_DEVICE);
}

/*
 * Clean up a connection ...
 */
static void
smbd_clean_connection(struct connection_struct *conn)
{
	rdma_disconnect(conn->cm_id);
	smbd_free_buffers(conn);

	ib_destroy_qp(conn->qp);
	ib_destroy_cq(conn->cq);
	ib_dealloc_pd(conn->pd);

	rdma_destroy_id(conn->cm_id);
}

/*
 * Handle a completion event ...
 */
static void handle_completion_event(struct ib_cq *cq, void *ctx)
{
	struct connection_struct *conn = ctx;
	int res = 0;
	struct ib_wc wc;

	printk(KERN_INFO "Handling a completion event ...\n");

	if (conn->cq != cq) {
		printk(KERN_ERR "The completion queue is wrong: %p, %p\n",
			conn->cq, cq);
		return;
	}

	/*
	 * Get all the available completions ...
	 */
	while ((res = ib_poll_cq(conn->cq, 1, &wc)) == 1) {
		switch (wc.opcode) {
		case IB_WC_SEND:
			printk(KERN_INFO "SEND completion ...\n");
			break;

		case IB_WC_RECV:
			printk(KERN_INFO "RECV completion ...\n");
			break;

		case IB_WC_RDMA_WRITE:
			printk(KERN_INFO "RDMA_WRITE completion ...\n");
			break;

		case IB_WC_RDMA_READ:
			printk(KERN_INFO "RDMA_READ completion ...\n");
			break;

		default:
			printk(KERN_ERR "%s:%d Unexpected completion: %d\n",
				__func__, __LINE__, wc.opcode);
			break;
		}
	}

	if (res) {
		printk(KERN_ERR "%s: ib poll error: %d\n", __func__, res);

		/* Nothing more can be done ... */
		goto done;
	}

	/*
	 * Make sure we get some more completion events.
	 */
	res = ib_req_notify_cq(conn->cq, IB_CQ_NEXT_COMP);
	if (res) {
		printk(KERN_ERR "Could not request notification: %d\n", res);
	}

done:
	return;
}

/*
 * Handle connection requests ... build a new connection and set up the 
 * protection domain, completion queue and queue pair, hang a receive and
 * accept the connection.
 */
static int
handle_connect_request(struct rdma_cm_id *cm_id,
		       struct smbd_device *smbd_dev)
{
	int res = 0;
	struct connection_struct *conn = NULL;
	struct ib_qp_init_attr conn_attr;
	struct rdma_conn_param cparam;
	struct ib_recv_wr *bad_wr;

	printk(KERN_INFO "Handling connection request ...\n");
	
	conn = kzalloc(sizeof(conn), GFP_KERNEL);
	if (!conn) {
		printk(KERN_ERR "Unable to allocate connection\n");
		return -ENOMEM;
	}

	conn->cm_id = cm_id;
	conn->dev = smbd_dev;

	/*
	 * Now allocate a protection domain, completion queue etc.
	 */
	conn->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(conn->pd)) {
		res = PTR_ERR(conn->pd);
		printk(KERN_ERR "Allocation of PD failed: %d\n", res);
		goto clean_conn;
	}

	conn->cq = ib_create_cq(cm_id->device, 
				handle_completion_event,
				NULL,
				conn,
				MAX_CQ_DEPTH * 2,
				0);
	if (IS_ERR(conn->cq)) {
		res = PTR_ERR(conn->cq);
		printk(KERN_ERR "Unable to create completion queue: %d\n",
			res);
		goto clean_pd;
	}

	/*
	 * Request notifies on that completion queue
	 */
	res = ib_req_notify_cq(conn->cq, IB_CQ_NEXT_COMP);
	if (res) {
		printk(KERN_ERR "Unable to request notifications: %d\n", res);
		goto clean_cq;
	}

	memset(&conn_attr, 0, sizeof(conn_attr));
	conn_attr.cap.max_send_wr = 10;
	conn_attr.cap.max_recv_wr = 10;
	conn_attr.cap.max_recv_sge = 2;
	conn_attr.cap.max_send_sge = 2;
	conn_attr.qp_type = IB_QPT_RC;
	conn_attr.send_cq = conn->cq;
	conn_attr.recv_cq = conn->cq;
	conn_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	res = rdma_create_qp(conn->cm_id, conn->pd, &conn_attr);
	if (res) {
		printk(KERN_ERR "Unable to create queue pair: %d\n", res);
		goto clean_cq;
	}

	conn->qp = conn->cm_id->qp;

	/*
	 * Setup the receive buff params for first receive
	 */
	conn->recv_buf_dma = ib_dma_map_single(conn->cm_id->device,
					       conn->recv_buf,
					       sizeof(conn->recv_buf),
					       DMA_FROM_DEVICE);
	res = ib_dma_mapping_error(conn->cm_id->device, conn->recv_buf_dma);
	if (res) {
		printk(KERN_ERR "Failed to create DMA mapping: %d\n", res);
		goto clean_dma;
	}

	conn->recv_mr = ib_get_dma_mr(conn->pd, IB_ACCESS_LOCAL_WRITE);

	/*
	 * Set up the work request ...
	 */
	conn->recv_sgl.addr = conn->recv_buf_dma;
	conn->recv_sgl.length = sizeof(conn->recv_buf);
	conn->recv_sgl.lkey = conn->recv_mr->lkey;

	conn->recv_wr.sg_list = &conn->recv_sgl;
	conn->recv_wr.num_sge = 1;
	conn->recv_wr.next = NULL;

	/*
	 * Post that receive before we accept
	 */
	res = ib_post_recv(conn->qp, &conn->recv_wr, &bad_wr);
	if (res) {
		printk(KERN_ERR "Unable to post a recv: %d\n", res);
		goto clean_recv;
	}

	/*
	 * Accept the request ...
	 */
	memset(&cparam, 0, sizeof(cparam));
	cparam.responder_resources = 1;
	cparam.initiator_depth = 1;

	res = rdma_accept(cm_id, &cparam);
	if (res) {
		printk(KERN_ERR "Unable to accept connection: %d\n", res);
		goto clean_recv;
	}

	cm_id->context = conn; /* How we find it again */
	mutex_lock(&smbd_dev->connection_list_mutex);
	list_add_tail(&smbd_dev->connection_list, &conn->connect_ent);
	mutex_unlock(&smbd_dev->connection_list_mutex);

	printk(KERN_INFO "Accepted the connection ...\n");

	return res;

clean_recv:

clean_dma:
	if (conn->recv_mr)
		ib_dereg_mr(conn->recv_mr);
	ib_dma_unmap_single(conn->cm_id->device,
			    conn->recv_buf_dma,
			    sizeof(conn->recv_buf),
			    DMA_FROM_DEVICE);
/*clean_qp: */
	ib_destroy_qp(conn->qp);
clean_cq:
	ib_destroy_cq(conn->cq);
clean_pd:
	ib_dealloc_pd(conn->pd);
clean_conn:
	rdma_destroy_id(conn->cm_id);
	kfree(conn);
	return res;
}

/*
 * Handle a connection disconnect. Simply clean the connection, delete it
 * from the list and free it.
 */
static int
handle_disconnect(struct rdma_cm_id *cma_id)
{
	int res = 0;
	struct connection_struct *conn = cma_id->context;
	struct smbd_device *smbd_dev = conn->dev;

	printk(KERN_INFO "Handling disconnect for %p\n", conn);

	smbd_free_buffers(conn);
	smbd_clean_connection(conn);

	mutex_lock(&smbd_dev->connection_list_mutex);
	list_del(&conn->connect_ent);
	mutex_unlock(&smbd_dev->connection_list_mutex);

	kfree(conn);

	return res;
}

/*
 * Handle CMA events ...
 */
static int
smbd_cma_handler(struct rdma_cm_id *cma_id,
		 struct rdma_cm_event *event)
{
	int res = 0;
	struct smbd_device *smbd_dev = cma_id->context;

	printk(KERN_INFO "cma_event type %d cma_id %p\n",
		event->event, cma_id);

	switch (event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		res = handle_connect_request(cma_id, smbd_dev);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		res = handle_disconnect(cma_id);
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:

	default:
		printk(KERN_ERR "Unknown event type: %d\n", event->event);
		break;
	}

	return res;
}

/*
 * Set up an RDMA CM Listen on our port ... and pass the device struct as
 * the context ...
 *
 * TODO: Generalize to IPV6 as well as IPV4.
 */
static int
setup_listen(struct smbd_device *smbd_dev)
{
	int res = 0;
	struct rdma_cm_id *smbd_lid = NULL;
	struct sockaddr_in sa;

	smbd_lid = rdma_create_id(smbd_cma_handler,
				  smbd_dev,
				  RDMA_PS_TCP,
				  IB_QPT_RC);
	if (IS_ERR(smbd_lid)) {
		res = PTR_ERR(smbd_lid);
		printk(KERN_ERR "rdma_create_id error %d\n", res);
		goto err;
	}

	smbd_dev->cm_lid = smbd_lid;

	/*
	 * Now bind to INADDR_ANY and our port.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = ntohs(SMB_DIRECT_PORT);

	res = rdma_bind_addr(smbd_lid, (struct sockaddr *)&sa);
	if (res) {
		printk(KERN_ERR "rdma_bind_addr error %d\n", res);
		goto unlisten;
	}

	printk(KERN_INFO "rdma_bind_addr done\n");

	/* TODO, allow the backlog to be tuned */
	res = rdma_listen(smbd_lid, 5);
	if (res) {
		printk(KERN_ERR "rdma_listen failed %d\n", res);
		goto unlisten;
	}

	printk(KERN_INFO "rdma_listen done, lid %p\n", smbd_lid);

	/* The callback function will handle things from here ... */

	return res;

unlisten:
	rdma_destroy_id(smbd_lid);
err:
	return res;
}

/*
 * Tear down the listens and any connections ...
 */
static int teardown_listen_and_connections(struct smbd_device *smbd_dev)
{
	int res = 0;
	struct connection_struct *conn, *temp;

	/* We need to go through the list of connections and drop them */
	mutex_lock(&smbd_dev->connection_list_mutex);
	list_for_each_entry_safe(conn, 
				temp,
				&smbd_dev->connection_list, 
				connect_ent) {

		rdma_disconnect(conn->cm_id);
		smbd_free_buffers(conn);

		ib_destroy_qp(conn->qp);
		ib_destroy_cq(conn->cq);
		ib_dealloc_pd(conn->pd);

		rdma_destroy_id(conn->cm_id);

		list_del(&conn->connect_ent);
		kfree(conn);
	}
	mutex_unlock(&smbd_dev->connection_list_mutex);

	if (smbd_dev->cm_lid)
		rdma_destroy_id(smbd_dev->cm_lid);

	return res;
}

static int smbd_proc_rd_open(struct inode *inode, struct file *file)
{
	return single_open(file, read_proc_stuff, NULL);
}

static const struct file_operations smbd_proc_rd_fops = {
	.open = smbd_proc_rd_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static int __init smbdirect_init(void)
{
	int res = 0;

	proc_create("driver/smbdirect", 0, NULL, &smbd_proc_rd_fops);

	/*
	 * Allocate a new device major number
	 */
	res = alloc_chrdev_region(&smbdirect_dev_no, 0, 1, "smbdirect");
	if (res < 0) {
		printk(KERN_ERR "Major number allocation failed\n");
		return res;
	}

	memset(&smbd_device, 0, sizeof(smbd_device));
	mutex_init(&smbd_device.connection_list_mutex);
	INIT_LIST_HEAD(&smbd_device.connection_list);
	cdev_init(&smbd_device.cdev, &smbd_fops);
	smbd_device.cdev.owner = THIS_MODULE;

	res = cdev_add(&smbd_device.cdev, smbdirect_dev_no, 1);
	if (res) {
		printk(KERN_ERR "Unable to add smbdirect device: %d\n", res);
		goto no_cdev;
	}

	/*
	 * This should be called in the open function when we have initialized
	 */
	res = setup_listen(&smbd_device);

no_cdev:
	return res;
}

static void __exit smbdirect_exit(void)
{
	(void)teardown_listen_and_connections(&smbd_device);
	cdev_del(&smbd_device.cdev);
	remove_proc_entry("driver/smbdirect", NULL);
}

MODULE_DESCRIPTION("smbdirect driver for Samba");
MODULE_VERSION("0.1");
MODULE_AUTHOR("rsharpe@samba.org");
MODULE_LICENSE("GPL");

module_init(smbdirect_init);
module_exit(smbdirect_exit);
