/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006-2009 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 
/*
 * kernel module for memory serving using RDMA modified from krping for COMEX use.
 */
 
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/time.h>
#include <linux/random.h>
#include <linux/sched.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "getopt.h"

#define PFX "krping: "

#define CHK(x) if(x){ printk("error\n");return;}
static int debug = 1;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG if (debug) printk

MODULE_AUTHOR("Chavit");
MODULE_DESCRIPTION("RDMA page frame transfer test");
MODULE_LICENSE("Dual BSD/GPL");

/*
 * Default max buffer size for IO...
 */
#define RPING_SQ_DEPTH 128
#define MAX_INLINE_PAYLOAD 162 //also say how big is the piggy
#define PAGESCOUNT 2048
#define RPING_BUFSIZE (4*1024*1024)
#include "COMEX_module_lib.h"		// for COMEX

enum mem_type {
	DMA = 1,
	FASTREG = 2,
	MW = 3,
	MR = 4
};

static const struct krping_option krping_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"addr6", OPT_STRING, 'A'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"server_inv", OPT_NOPARAM, 'I'},
 	{"bw", OPT_NOPARAM, 'B'},
 	{"txdepth", OPT_INT, 'T'},
 	{"poll", OPT_NOPARAM, 'P'},
 	{"local_dma_lkey", OPT_NOPARAM, 'Z'},
 	{"read_inv", OPT_NOPARAM, 'R'},
 	{"node_ID", OPT_INT, 'i'},			// for COMEX
 	{"n_nodes", OPT_INT, 'n'},
 	{"total_pages", OPT_INT, 't'},
 	{"writeOut_buff", OPT_INT, 'w'},
 	{"readIn_buff", OPT_INT, 'r'},
	{"proc_name", OPT_STRING, 'o'},
	{NULL, 0, 0}
};

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

static DEFINE_MUTEX(krping_mutex);

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;


/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
  RDMA_READY,
	ERROR
};

struct buffer_info {
	uint64_t buf; //not actually buffer ptr, but buffer addr array ptr.
	uint32_t rkey;
	uint32_t size; 
	//uint32_t instanceno; //what if i don't need it?
};
union bufferx{
	struct buffer_info buffer_info;
	char piggy[MAX_INLINE_PAYLOAD];
};
/*
 *
 */
 
struct krping_sharedspace {
	struct scatterlist sg[PAGESCOUNT]; //scatter list, max 8gb for now
	char *bufferpages[PAGESCOUNT]; //our buffer, normal addr
	uint64_t dmapages[PAGESCOUNT]; //our buffer, dma addr
	int numbigpages; //number of big pages that it actually use
};
 
/*
 * Control block struct.
 */
struct krping_cb {
	int server;			/* 0 iff client */
	struct ib_cq *cq_send;
  struct ib_cq *cq_recv;
	struct ib_pd *pd;
	struct ib_qp *qp;
  struct krping_sharedspace *bigspace;
	enum mem_type mem;
	struct ib_mr *dma_mr;
  
	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_send_wr invalidate_wr;
	int server_invalidate;
	int read_inv;
	u8 key;

	struct ib_mw *mw;
	struct ib_mw_bind bind_attr;

	struct ib_recv_wr rq_wr;	/* recv work request record */
	struct ib_sge recv_sgl;		/* recv single SGE */
	union bufferx recv_buf; /* malloc'd buffer */
	u64 recv_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(recv_mapping)
	struct ib_mr *recv_mr;

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	union bufferx send_buf;/* single send buf */
	u64 send_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ib_mr *send_mr;

	struct ib_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;		/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	u64  rdma_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(rdma_mapping)
  DECLARE_PCI_UNMAP_ADDR(dmabuf_mapping)
  DECLARE_PCI_UNMAP_ADDR(ptable_mapping)
	uint32_t remote_rkey;		/* remote guys RKEY */
	uint32_t remote_len;		/* remote guys LEN */ //resize?

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
  struct semaphore sem_exit;
  struct semaphore sem_verb;
  struct semaphore sem_read;
  struct semaphore sem_write;
	uint16_t port;			/* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	char *addr_str;			/* dst addr string */
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */
	int verbose;			/* verbose logging */
	int count;			/* ping count */
	int size;			/* ping data size */
	int poll;			/* poll or block for rlat test */
	int txdepth;			/* SQ depth */
	int local_dma_lkey;		/* use 0 for lkey */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on server side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;	
  //main big memory
	uint64_t remote_addr[PAGESCOUNT];		// remote buffer, normal addr
  int numpages;
  //for buffer exchange
  u64 ptable_dma_addr;
  u64 remote_dmabuf_addr;
  
};
// regis memory
static int regis_bigspace(struct krping_sharedspace *bigspace, int num_bigpages){
	int i;
	bigspace->numbigpages = num_bigpages;
	for(i=0; i<num_bigpages; i++){
		//sg_set_page(&cb->sg[i],alloc_pages( GFP_KERNEL, 10),4*1024*1024,0); // choice A, get page directly
		bigspace->bufferpages[i] = kmalloc(RPING_BUFSIZE, GFP_KERNEL); // choice B, get buffer and addr
		sg_set_buf(&bigspace->sg[i], bigspace->bufferpages[i], RPING_BUFSIZE);
	}
}

//big page align in 4MB chunks
static uint64_t translate_useraddr(struct krping_cb *cb,uint64_t offset){
	return cb->bigspace->bufferpages[offset/RPING_BUFSIZE]+(offset%RPING_BUFSIZE);
}

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", 
			       ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		//if (!cb->server) {
			cb->state = CONNECTED;
		//}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
  //break; //fall through, no break
	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT ...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}
static int do_write(struct krping_cb *cb,u64 local_offset,u64 remote_offset,u64 size){
  int ret;
  struct ib_send_wr *bad_wr;
  uint64_t pageno,pageoffset;
  DEBUG_LOG("RDMA WRITE\n");
  printk("localoffset=%lld remoteoffset=%lld\n",local_offset,remote_offset);
	cb->rdma_sgl.lkey = cb->dma_mr->rkey; //no lkey?
  //change
  cb->rdma_sq_wr.opcode = IB_WR_RDMA_WRITE;
  cb->rdma_sgl.addr = cb->rdma_dma_addr+local_offset;
	cb->rdma_sq_wr.sg_list->length = size;
  //
  pageno=remote_offset/RPING_BUFSIZE;
  pageoffset=remote_offset%RPING_BUFSIZE;
  if(pageoffset+size>RPING_BUFSIZE){
    printk("\nALERT, BUFFER MISALIGNMENT FOUND\n\n\n");
  }
	cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr[pageno]+pageoffset; 
  //
  printk("pageno=%lld pageoffset=%lld\n",pageno,pageoffset);
  ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post read err %d\n", ret);
			return ret;
		}
  return 0;
}

static int do_read(struct krping_cb *cb,u64 local_offset,u64 remote_offset,u64 size){
  int ret;
  struct ib_send_wr *bad_wr;
  uint32_t pageno,pageoffset;
  DEBUG_LOG("RDMA READ\n");
	cb->rdma_sgl.lkey = cb->dma_mr->rkey; //no lkey?
  //change
  cb->rdma_sq_wr.opcode = IB_WR_RDMA_READ;
  cb->rdma_sgl.addr = cb->rdma_dma_addr+local_offset;
	cb->rdma_sq_wr.sg_list->length = size;
  //
  pageno=remote_offset/RPING_BUFSIZE;
  pageoffset=remote_offset%RPING_BUFSIZE;
  if(pageoffset+size>RPING_BUFSIZE){
    printk("\nALERT, BUFFER MISALIGNMENT FOUND\n\n\n");
  }
  cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr[pageno]+pageoffset; 
  //
  printk("pageno=%d pageoffset=%d\n",pageno,pageoffset);
  ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post read err %d\n", ret);
			return ret;
		}
  return 0;
}
// internal call,
static int do_read_bufferptr(struct krping_cb *cb,uint64_t theirptrs,int numpages){
  int ret;
  printk("theirptrs=%llx numpages=%d\n",theirptrs,numpages);
  struct ib_send_wr *bad_wr;
  cb->rdma_sgl.lkey = cb->dma_mr->rkey; //no lkey?
  cb->rdma_sq_wr.opcode = IB_WR_RDMA_READ;
  cb->rdma_sgl.addr = (uint64_t)cb->remote_dmabuf_addr; //at offset 0
  cb->rdma_sq_wr.sg_list->length = sizeof(char*)*numpages;
  cb->rdma_sq_wr.wr.rdma.remote_addr =theirptrs;
  
  ret = ib_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post read err %d\n", ret);
			return ret;
		}
  return 0;
}
// internal call, will improve
static int deep_send(struct krping_cb *cb, u64 imm){
	struct ib_send_wr *bad_wr;
  int ret;
  
  
	//cb->send_sgl.addr = cb->send_dma_addr;
	//cb->send_sgl.length = sizeof cb->send_buf;
 //cb->send_sgl.lkey = cb->dma_mr->lkey; //
  cb->sq_wr.ex.imm_data=imm; 
  //dma_sync_single_for_device(cb->pd->device->dma_device, cb->send_dma_addr, sizeof(cb->send_buf), DMA_TO_DEVICE);
  ret=ib_post_send(cb->qp, &cb->sq_wr, &bad_wr); //here 1
  if(ret){
    printk("send error");
  }
  return ret;
}
static int universal_send(struct krping_cb *cb, u64 imm, char* addr, u64 size){
  void *info = &cb->send_buf;
  memcpy(info,addr,size);
  return deep_send(cb, imm);
}
//no more one send, then tell RDMA write whole things
static int send_buffer_info(struct krping_cb *cb)
{
  int i,ret;
  struct buffer_info *info = (struct buffer_info *) &(cb->send_buf);
  //init buffer addr to read
  for(i=0;i<cb->bigspace->numbigpages;i++){
    cb->bigspace->dmapages[i]=sg_dma_address(&cb->bigspace->sg[i]);
  }
  
  DEBUG_LOG("about to send buffer info\n");
  
  //info->buf = htonll(cb->rdma_dma_addr);
  info->buf = htonll(cb->ptable_dma_addr);
  info->rkey = htonl(cb->dma_mr->rkey); // cb->rdma_mr->rkey; // change!
  info->size = htonl(PAGESCOUNT);
  DEBUG_LOG("send RDMA buffer table addr %llx rkey %x len =%d pages\n", (uint64_t)cb->ptable_dma_addr, cb->dma_mr->rkey, PAGESCOUNT);
  ret= deep_send(cb, 2);
  ret=down_interruptible(&cb->sem_verb);
  DEBUG_LOG("send buffer info success wait for recv\n");
  ret=down_interruptible(&cb->sem_read);
  if (ret) {
    printk(KERN_ERR PFX "post read err %d\n", ret);
    return ret;
  }
  
  wait_event_interruptible(cb->sem, cb->state >= RDMA_READY);
  DEBUG_LOG("exchange buffer info success\n");
  //
  /*
  DEBUG_LOG("the remote buffer addrs are\n");
  for(i=0;i<100;i++){
    DEBUG_LOG("%d = %llx\n",i,cb->remote_addr[i]);
  }
  */
  return 0;
}
static int universal_recv_handler(struct krping_cb *cb, struct ib_wc *wc){
  int i,ret;
  if(wc->opcode==IB_WC_RECV){
    if(wc->wc_flags & IB_WC_WITH_IMM){
      switch(wc->ex.imm_data){
        case 2: //set buffers info
          cb->remote_len  = ntohl(cb->recv_buf.buffer_info.size)*RPING_BUFSIZE; //checkback, need hll? , do we really send big data
          cb->remote_rkey = ntohl(cb->recv_buf.buffer_info.rkey);
          cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
          /* // old
          cb->remote_addr[ntohl((cb->recv_buf.buffer_info.slot))] = ntohll(cb->recv_buf.buffer_info.buf);
          DEBUG_LOG("recv RDMA buffer addr %llx rkey %x len %d\n", cb->remote_addr[ntohl((cb->recv_buf.buffer_info.slot))], cb->remote_rkey, cb->remote_len);
          */
          do_read_bufferptr(cb,ntohll(cb->recv_buf.buffer_info.buf),ntohl(cb->recv_buf.buffer_info.size));
          
          //
          cb->state=RDMA_READY;
          
          wake_up_interruptible(&cb->sem);
          break;
        case 5: //set buffers info
          DEBUG_LOG("recv exit");
          up(&cb->sem_exit);
          break;  
        default:
          printk("unexpected,unhandled immediate received=%d %s\n",wc->ex.imm_data,cb->recv_buf.piggy);
      }
    }else{
      printk("call recv handler but no imm\n");
    }
  }else{
      printk("call recv handler but not recv event\n");
  }
  return 0;
}

static void krping_cq_event_handler_send(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq_send != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
		ib_req_notify_cq(cb->cq_send, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq_send, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n");
      up(&cb->sem_verb);
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			up(&cb->sem_write);
			break;

		case IB_WC_RDMA_READ:
    
			DEBUG_LOG("rdma read completion\n");
			up(&cb->sem_read);
			break;
		default:
			printk(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

static void krping_cq_event_handler_recv(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq_recv != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
		ib_req_notify_cq(cb->cq_recv, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq_recv, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			ret = universal_recv_handler(cb, &wc);
			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}
  //repost recv req(slot later)
			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			//wake_up_interruptible(&cb->sem);
			break;

		default:
			printk(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}
static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}
  wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
  if (cb->state == ERROR) {
    printk(KERN_ERR PFX "wait for CONNECTED state %d\n", 
      cb->state);
    return -1;
  }
	
	return 0;
}

static void krping_setup_wr(struct krping_cb *cb)
{
  //DEBUG_LOG("CALLED krping_setup_wr\n");
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
  cb->recv_sgl.lkey = cb->dma_mr->lkey; // cb->recv_mr->lkey; //
  
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

//send structures that have never changed, thus init here only once
	cb->sq_wr.opcode = IB_WR_SEND_WITH_IMM;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED ; //inline? IB_SEND_INLINE

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
  cb->send_sgl.lkey = cb->dma_mr->lkey; //cb->send_mr->lkey; //
  
  cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;
    
  
  cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
  cb->rdma_sq_wr.next = NULL;
  //cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
  cb->rdma_sq_wr.send_flags = IB_SEND_SIGNALED;
  cb->rdma_sq_wr.num_sge = 1;
  
	
  DEBUG_LOG("setup_wr done\n");
}

static int krping_setup_buffers(struct krping_cb *cb)
{
	int ret,i;
  //char * test[2000];
  //struct ib_mr * dmatest[2000];
  int tests;
  //struct ib_fast_reg_page_list *pl;
  //int plen = (((4*1024*1024 - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
  
	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);
  //send/recv buffer do dma only
  //recv_buf,send_buf is CPU ptr, recv_dma_addr and send_dma_addr is their
	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device, &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,&cb->send_buf,sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

  
  //printk("recv %llx %llx, send %llx %llx",cb->recv_dma_addr,cb->recv_buf,cb->send_dma_addr,cb->send_buf);
  cb->dma_mr = ib_get_dma_mr(cb->pd, IB_ACCESS_LOCAL_WRITE 
           |IB_ACCESS_REMOTE_READ|IB_ACCESS_REMOTE_WRITE);//|IB_ACCESS_LOCAL_READ
  if (IS_ERR(cb->dma_mr)) {
    DEBUG_LOG(PFX "reg_dmamr failed\n");
    ret = PTR_ERR(cb->dma_mr);
    goto bail;
  }
  DEBUG_LOG("done creating verb buffers\n");
  //////////////// rdma part
  
  cb->rdma_buf = kmalloc(cb->size, GFP_KERNEL); 
	if (!cb->rdma_buf) {
		DEBUG_LOG(PFX "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}
  cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device, 
			       cb->rdma_buf, cb->size, 
			       DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
  
  // 3, try scatter, gatter list
  //set dma mask to 64, so more mem can be registerd
  if(dma_set_mask(cb->pd->device->dma_device, 64)){
    DEBUG_LOG("mask accepted\n");
  }
  /* // do it in krping_doit function, as this will be shared, not sure if sg can be shared
  for(i=0;i<cb->numpages;i++){
     //sg_set_page(&cb->sg[i],alloc_pages( GFP_KERNEL, 10),4*1024*1024,0); // choice A, get page directly
    cb->bigspace->bufferpages[i]=kmalloc(RPING_BUFSIZE, GFP_KERNEL); // choice B, get buffer and addr
    sg_set_buf(&cb->bigspace->sg[i],cb->bigspace->bufferpages[i],RPING_BUFSIZE);
  }
  */
  tests=dma_map_sg(cb->pd->device->dma_device,cb->bigspace->sg,cb->bigspace->numbigpages,DMA_BIDIRECTIONAL);
  /*
  DEBUG_LOG("some of my addr\n");
  for(i=0;i<100;i++){
    DEBUG_LOG("sg_dma_map=%llx regular addr =%lx sg_dma_len=%u\n",sg_dma_address(&(cb->sg[i])),cb->sg[i].page_link,sg_dma_len(&(cb->sg[i])));
  }
  */
  DEBUG_LOG("done rdma malloc\n");

  //page table ptr, to read ptr from, will initialize in dma_buffer send, hard coded size for here, can use less.
  cb->ptable_dma_addr = dma_map_single(cb->pd->device->dma_device,&cb->bigspace->dmapages,sizeof(char*)*PAGESCOUNT, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, ptable_mapping, cb->ptable_dma_addr);

  cb->remote_dmabuf_addr=dma_map_single(cb->pd->device->dma_device,&cb->remote_addr,sizeof(char*)*PAGESCOUNT, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, dmabuf_mapping, cb->remote_dmabuf_addr);

  
  //*/
  //MR mode
  /*
  buf.addr = cb->rdma_buf;
  buf.size = cb->size;
  iovbase = cb->rdma_buf;
  cb->rdma_dma_addr = ib_reg_phys_mr(cb->pd, &buf, 1, 
           IB_ACCESS_REMOTE_READ| 
           IB_ACCESS_REMOTE_WRITE, 
           &iovbase);
  */
  //
/*
  unsigned flags = IB_ACCESS_REMOTE_READ|IB_ACCESS_REMOTE_WRITE;
  buf.addr = cb->start_dma_addr;
  buf.size = cb->size;
  DEBUG_LOG(PFX "start buf dma_addr %llx size %d\n", 
    buf.addr, (int)buf.size);
  iovbase = cb->start_dma_addr;
  cb->start_mr = ib_reg_phys_mr(cb->pd, &buf, 1, 
           flags,
           &iovbase);
*/
  //
  
  //
  
	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
bail:
	if (cb->mw && !IS_ERR(cb->mw))
		ib_dealloc_mw(cb->mw);
	if (cb->page_list && !IS_ERR(cb->page_list))
		ib_free_fast_reg_page_list(cb->page_list);
  if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->recv_mr && !IS_ERR(cb->recv_mr))
		ib_dereg_mr(cb->recv_mr);
	if (cb->send_mr && !IS_ERR(cb->send_mr))
		ib_dereg_mr(cb->send_mr);
	if (cb->rdma_buf)
		kfree(cb->rdma_buf);
	return ret;
}

static void krping_free_buffers(struct krping_cb *cb)
{
  int i;
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);
	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->send_mr)
		ib_dereg_mr(cb->send_mr);
	if (cb->recv_mr)
		ib_dereg_mr(cb->recv_mr);
	if (cb->mw)
		ib_dealloc_mw(cb->mw);

	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, recv_mapping),
			 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, rdma_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
  dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, ptable_mapping),
			 sizeof(char*)*PAGESCOUNT, DMA_BIDIRECTIONAL);
  dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, dmabuf_mapping),
			 sizeof(char*)*PAGESCOUNT, DMA_BIDIRECTIONAL);
  dma_unmap_sg(cb->pd->device->dma_device,
			 cb->bigspace->sg,
			 cb->bigspace->numbigpages, DMA_BIDIRECTIONAL);     
       
	kfree(cb->rdma_buf);
  
  for(i=0;i<PAGESCOUNT;i++){
    kfree((void*)cb->bigspace->bufferpages[i]);
  }
  
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 128; //modified
	init_attr.cap.max_recv_sge = 2;
	init_attr.cap.max_send_sge = 2; 
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq_send;
	init_attr.recv_cq = cb->cq_recv;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq_send);
  ib_destroy_cq(cb->cq_recv);
	ib_dealloc_pd(cb->pd);
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	cb->pd = ib_alloc_pd(cm_id->device);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->cq_recv = ib_create_cq(cm_id->device, krping_cq_event_handler_recv, NULL,
			      cb, cb->txdepth * 2, 0);
  cb->cq_send = ib_create_cq(cm_id->device, krping_cq_event_handler_send, NULL,
			      cb, cb->txdepth * 2, 0);          
	if (IS_ERR(cb->cq_send)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq_send);
		goto err1;
	}
  if (IS_ERR(cb->cq_recv)) {
    printk(KERN_ERR PFX "ib_create_cq failed\n");
    ret = PTR_ERR(cb->cq_recv);
    goto err1;
	}
	DEBUG_LOG("created cq send %p recv %p\n", cb->cq_send,cb->cq_recv);
  ret = cb->cq_recv->device->req_notify_cq(cb->cq_send, IB_CQ_NEXT_COMP);
  if (ret) {
    printk(KERN_ERR PFX "ib_create_cq failed\n");
    goto err2;
  }
	ret = cb->cq_recv->device->req_notify_cq(cb->cq_recv, IB_CQ_NEXT_COMP);
  if (ret) {
    printk(KERN_ERR PFX "ib_create_cq failed\n");
    goto err3;
  }
	ret = krping_create_qp(cb); 
	if (ret) {
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;
  err3:
  ib_destroy_cq(cb->cq_recv);
  err2:
	ib_destroy_cq(cb->cq_send);
  err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}



static void krping_test_server(struct krping_cb *cb)
{
	int ret;
	char t[6]="a";

	//exchange buffer info
	ret = send_buffer_info(cb);
	if (ret) {
		printk(KERN_ERR PFX "buffer info err %d\n", ret);
		return;
	}

	//test send
	CHK(universal_send(cb, 99,t, 4)) 
	ret=down_interruptible(&cb->sem_verb);
	t[0]++;

	//test read
	printk("issue read\n");
	CHK(do_read(cb,0,0,24) )
	printk("wait sem read\n");
	ret=down_interruptible(&cb->sem_read);
	//dma_sync_single_for_cpu(cb->pd->device->dma_device, cb->rdma_dma_addr, sizeof(cb->rdma_buf), DMA_FROM_DEVICE);

	printk("string= %s\n",(cb->rdma_buf) );


	//test write, otherside will check at exit
	sprintf(cb->rdma_buf+16,"whataburger11 ");
	//dma_sync_single_for_device(cb->pd->device->dma_device, cb->rdma_dma_addr, sizeof(cb->rdma_buf), DMA_TO_DEVICE);

	CHK(do_write(cb,16,16,24) )
	ret=down_interruptible(&cb->sem_write);
	printk("done\n");

//	CHK(universal_send(cb, 5,t, 4)) //test kill signal
	ret=down_interruptible(&cb->sem_exit);
}



static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (cb->addr_type == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	} else if (cb->addr_type == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
	}
}

static int krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;


	fill_sockaddr(&sin, cb);

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		printk(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST) {
		printk(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	return 0;
}

static void krping_run_server(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_server(cb); 
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->child_cm_id); 
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_accept(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	krping_test_server(cb);
  
	rdma_disconnect(cb->child_cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
err0:
	rdma_destroy_id(cb->child_cm_id);
}

static void krping_test_client(struct krping_cb *cb)
{
	int start, ret;
	char t[200]="zxg";
	start = 65;

	//exchange buffer info
	sprintf(cb->bigspace->bufferpages[0],"rdma-ping-%d: ", 1); //someone will read here
	printk("rdma buffer= %s\n",(char*)(cb->bigspace->bufferpages[0]) );
	//dma_sync_sg_for_device(cb->pd->device->dma_device, cb->sg, PAGESCOUNT, DMA_TO_DEVICE);
	ret = send_buffer_info(cb); 
	if (ret) {
		printk(KERN_ERR PFX "buffer info error %d\n", ret);
		return;
	}
	/*
	CHK(universal_send(cb, 99,t, 4)) 
	ret=down_interruptible(&cb->sem_verb);
	t[0]--;


	//CHK(universal_send(cb, 99,&t, 4) ); //send
	*/
//	CHK(universal_send(cb, 5, t, 4)) //test kill signal
	ret=down_interruptible(&cb->sem_exit);
	printk("unlocked\n");
	//dma_sync_sg_for_cpu(cb->pd->device->dma_device, cb->sg, PAGESCOUNT, DMA_FROM_DEVICE);
	printk("string= %s\n",(char*)(cb->bigspace->bufferpages[0]+16) ); //someone write here //testbug
}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

static int krping_bind_client(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX 
		       "addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}


	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static void krping_run_client(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_client(cb); 
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->cm_id); 
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb); 
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}
	krping_test_client(cb);
  
  
	rdma_disconnect(cb->cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
}

int krping_doit(char *cmd)
{
	struct krping_cb *cb;
	struct krping_sharedspace *bigspaceptr;
	int op;
	int ret = 0;
	char *optarg;
	unsigned long optint;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	///
	bigspaceptr = kzalloc(sizeof(*bigspaceptr), GFP_KERNEL);
	if (!bigspaceptr)
		return -ENOMEM;
	cb->bigspace = bigspaceptr;
	regis_bigspace(bigspaceptr, PAGESCOUNT);
	///
	
	mutex_lock(&krping_mutex);
	list_add_tail(&cb->list, &krping_cbs);
	mutex_unlock(&krping_mutex);

	cb->server = -1;
	cb->state = IDLE;
	cb->size = RPING_BUFSIZE;
	cb->txdepth = RPING_SQ_DEPTH;
	cb->mem = DMA;
	
	init_waitqueue_head(&cb->sem);
	sema_init(&cb->sem_exit,0);
	sema_init(&cb->sem_verb,0);
	sema_init(&cb->sem_read,0);
	sema_init(&cb->sem_write,0);
	
	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg, &optint)) != 0) {
		switch (op) {
			case 'a':
				cb->addr_str = optarg;
				in4_pton(optarg, -1, cb->addr, -1, NULL);
				cb->addr_type = AF_INET;
				DEBUG_LOG("ipaddr (%s)\n", optarg);
				break;
			case 'A':
				cb->addr_str = optarg;
				in6_pton(optarg, -1, cb->addr, -1, NULL);
				cb->addr_type = AF_INET6;
				DEBUG_LOG("ipv6addr (%s)\n", optarg);
				break;
			case 'p':
				cb->port = htons(optint);
				DEBUG_LOG("port %d\n", (int)optint);
				break;
			case 's':
				cb->server = 1;
				DEBUG_LOG("server\n");
				break;
			case 'c':
				cb->server = 0;
				DEBUG_LOG("client\n");
				break;
			case 'v':
				cb->verbose++;
				DEBUG_LOG("verbose\n");
				break;
			case 'i':
				node_ID = (int)optint;			// for COMEX
				break;
			case 'n':
				n_nodes = (int)optint;
				break;
			case 't':
				total_pages = (int)optint;
				break;
			case 'w':
				writeOut_buff = (int)optint;
				break;
			case 'r':
				readIn_buff = (int)optint;
				break;
			case 'o':
				strcpy(proc_name, optarg);
				break;
			default:
				printk(KERN_ERR PFX "unknown opt %s\n", optarg);
				ret = -EINVAL;
				break;
		}
	}
	
	if (ret)
		goto out;

	if (cb->server == -1) {
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	cb->cm_id = rdma_create_id(krping_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}

	global_CB = cb;		// for COMEX
	COMEX_init();
	
	if (cb->server)
		krping_run_server(cb);
	else
		krping_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out:
	mutex_lock(&krping_mutex);
	list_del(&cb->list);
	mutex_unlock(&krping_mutex);
	kfree(cb);
	return ret;
}

/*
 * Read proc returns stats for each device.
 */
static int krping_read_proc(struct seq_file *seq, void *v)
{
	struct krping_cb *cb;
	int num = 1;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	DEBUG_LOG(KERN_INFO PFX "proc read called...\n");
	mutex_lock(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list) {
		if (cb->pd) {
			seq_printf(seq,"good\n");
		} else {
			seq_printf(seq, "%d listen\n", num++);
		}
	}
	mutex_unlock(&krping_mutex);
	module_put(THIS_MODULE);
	return 0;
}

/*
 * Write proc is used to start a ping client or server.
 */
static ssize_t krping_write_proc(struct file * file, const char __user * buffer,
		size_t count, loff_t *ppos)
{
	char *cmd;
	int rc;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		printk(KERN_ERR PFX "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	DEBUG_LOG(KERN_INFO PFX "proc write |%s|\n", cmd);
	rc = krping_doit(cmd);
	kfree(cmd);
	module_put(THIS_MODULE);
	if (rc)
		return rc;
	else
		return (int) count;
}

static int krping_read_open(struct inode *inode, struct file *file)
{
        //return single_open(file, krping_read_proc, inode->i_private);
        return single_open(file, krping_read_proc, inode->i_private);
}
struct file_operations krping_ops = {
	.owner   = THIS_MODULE,
	.open    = krping_read_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write   = krping_write_proc,
  };

static int __init krping_init(void)
{
	DEBUG_LOG("krping_init\n");
	krping_proc = proc_create("krping", 0666, NULL, &krping_ops);
	if (krping_proc == NULL) {
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit krping_exit(void)
{
	DEBUG_LOG("krping_exit\n");
	remove_proc_entry("krping", NULL);
}

module_init(krping_init);
module_exit(krping_exit); //need to be removed