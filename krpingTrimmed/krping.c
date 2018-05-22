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
#include <linux/kthread.h>
//#include<linux/semaphore.h>
#include <linux/delay.h> // require for sleep?
#include <asm/atomic.h>
#include <asm/pci.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "getopt.h"
#include "myconfig.h"

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
 //CAN NOT USE 256, it's error!
#define RPING_SQ_DEPTH 127 
#define MAX_INLINE_PAYLOAD 162 //also say how big is the piggy
//#define MAXPAGESCOUNT 4096 // maximum page counts support, for places like pagetable pre-defined
#define RPING_BUFSIZE (4*1024*1024)
#define VERB_RECV_SLOT 64
#define VERB_SEND_SLOT 24

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
	RECV_INFO,
	RDMA_READY,
	ERROR
};

struct buffer_info {
	uint64_t buf; //not actually buffer ptr, but buffer addr array ptr.
	uint32_t rkey;
	uint32_t size; 
	uint32_t instanceno; //what if i don't need it?
};
union bufferx{
	struct buffer_info buffer_info;
	char piggy[MAX_INLINE_PAYLOAD];
};
/*
 *
 */
 
struct krping_sharedspace{
	struct   scatterlist *sg; 	//scatter list, max 8gb for now
	char    **bufferpages; 		//our buffer, normal addr
	uint64_t *dmapages; 			//our buffer, dma addr
	int      numbigpages; 					//number of big pages that it actually use
};
struct krping_cb;

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
	struct ib_mr *dma_mr;

	int read_inv;
	u8 key;
	int exitstatus;
	struct ib_recv_wr rq_wr[VERB_RECV_SLOT];	/* recv work request record */ //+1 from special Ack's slot?
	struct ib_sge recv_sgl[VERB_RECV_SLOT];		/* recv single SGE */ //+1 from special Ack's slot?
	union bufferx recv_buf[VERB_RECV_SLOT]; 	/* malloc'd buffer */
	//atomic_t v_recv_counter;
	int v_recv_counter;

	struct ib_send_wr sq_wr[VERB_SEND_SLOT];	/* send work requrest record */
	struct ib_sge send_sgl[VERB_SEND_SLOT];
	union bufferx send_buf[VERB_SEND_SLOT];
	int vslotusing;

	u64 recv_dma_addr; //single position only, will calculate address offset later
	u64 send_dma_addr;

	DECLARE_PCI_UNMAP_ADDR(recv_mapping)
	struct ib_mr *recv_mr;

	DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ib_mr *send_mr;

	char *rdma_buf;				/* used as rdma sink */
	DECLARE_PCI_UNMAP_ADDR(dmabuf_mapping)
	DECLARE_PCI_UNMAP_ADDR(ptable_mapping)
	uint32_t remote_rkey;		/* remote guys RKEY */
	uint32_t remote_pcount;		/* remote guys LEN */ //number of remote pages

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
	struct semaphore sem_signal_ack;
	struct semaphore sem_verb_ack;
	struct semaphore sem_verb_mutex;
	struct semaphore sem_read_able;
	struct semaphore sem_read;
	struct semaphore sem_write_able;
	struct semaphore sem_ready;
	
	uint16_t port;			/* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	char *addr_str;			/* dst addr string */
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */
	int verbose;			/* verbose logging */
	int count;				/* ping count */
	int size;				/* ping data size */
	int txdepth;			/* SQ depth */
	int local_dma_lkey;		/* use 0 for lkey */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
								/* listener on server side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;	
	
	//for buffer exchange
	uint64_t ptable_dma_addr; //dma addr for local pagetable
	uint64_t  remote_dmabuf_ptr; //dma addr of remote side's page table

	uint64_t *remote_addr;		// local copy of remote buffer page table, normal addr
	uint64_t remote_dmabuf_addr; //local copy of remote buffer page table,dma addr

	//indexes
	int cbindex;
	int mynodeID;
	int remotenodeID;
};
struct krping_cb **cbs;

#include "COMEX_module_lib.h"		// for COMEX
// regis memory
static int regis_bigspace(struct krping_sharedspace *bigspace,int num_bigpages)
{
	int i;
	bigspace->numbigpages = num_bigpages;
	bigspace->sg          = kzalloc(sizeof(struct   scatterlist) *num_bigpages,GFP_KERNEL);
	bigspace->bufferpages = kmalloc(sizeof(char*) *num_bigpages,GFP_KERNEL);
	bigspace->dmapages    = kzalloc(sizeof(uint64_t)*num_bigpages,GFP_KERNEL);

	for(i=0; i<num_bigpages; i++){
		bigspace->bufferpages[i] = NULL;	// Zero initialize
	}
	for(i=0; i<num_bigpages; i++){
		//sg_set_page(&cb->sg[i],alloc_pages( GFP_KERNEL, 10),4*1024*1024,0); // choice A, get page directly
		bigspace->bufferpages[i] = kmalloc(RPING_BUFSIZE, GFP_KERNEL); // choice B, get buffer and addr
		if(!bigspace->bufferpages[i]){
		  DEBUG_LOG("Kmalloc fail\n");
		  return -1;
		}
		sg_set_buf(&bigspace->sg[i], bigspace->bufferpages[i], RPING_BUFSIZE);
	}
	return 0;
}

//big page align in 4MB chunks
static uint64_t translate_useraddr(struct krping_cb *cb, uint64_t offset){
	if(cb->bigspace->bufferpages[offset/RPING_BUFSIZE] == NULL){
		return NULL;
	}
	return (uint64_t)cb->bigspace->bufferpages[offset/RPING_BUFSIZE]+(offset%RPING_BUFSIZE);
}

static int krping_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)

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
		cb->state = CONNECTED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event, event->status);
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

static int do_write(struct krping_cb *cb,u64 local_offset,u64 remote_offset,u64 size)
{
	int ret;
	struct ib_send_wr *bad_wr;
	struct ib_send_wr rdma_sq_wr_copy= {0};	
	struct ib_sge rdma_sgl_new= {0};		
	struct semaphore sem_write;
	
	if(( local_offset%RPING_BUFSIZE + size > RPING_BUFSIZE)||
	   (remote_offset%RPING_BUFSIZE + size > RPING_BUFSIZE)){ //chk misalignment
		DEBUG_LOG("\nlocal %lu %lu %lu ",  local_offset, RPING_BUFSIZE, size);
		DEBUG_LOG("\nremot %lu %lu %lu ", remote_offset, RPING_BUFSIZE, size);
		DEBUG_LOG("\nALERT, BUFFER MISALIGNMENT FOUND\n");
		return 1;
	}else if(remote_offset/RPING_BUFSIZE>cb->remote_pcount){
		DEBUG_LOG("\nALERT WRITE, %ld %ld %ld\n", local_offset, remote_offset, size);
		return 1;
    }else{
		sema_init(&sem_write,0);  
	//	memcpy(&rdma_sq_wr_copy,&cb->rdma_sq_wr_proto,sizeof(struct ib_send_wr));
	//	replace with
		rdma_sq_wr_copy.wr.rdma.rkey = cb->remote_rkey;
		rdma_sq_wr_copy.next		 = NULL;
		rdma_sq_wr_copy.send_flags	 = IB_SEND_SIGNALED;
		rdma_sq_wr_copy.num_sge 	 = 1;

		rdma_sq_wr_copy.opcode				= IB_WR_RDMA_WRITE;
		rdma_sq_wr_copy.wr_id				= (uint64_t)&sem_write;
		rdma_sq_wr_copy.sg_list				= &rdma_sgl_new; 
		rdma_sgl_new.lkey					= cb->dma_mr->rkey;
		rdma_sq_wr_copy.sg_list->length 	= size; //
		rdma_sgl_new.addr 					= cb->bigspace->dmapages[local_offset/RPING_BUFSIZE]+(local_offset%RPING_BUFSIZE); //
		rdma_sq_wr_copy.wr.rdma.remote_addr = cb->remote_addr[remote_offset/RPING_BUFSIZE]+(remote_offset%RPING_BUFSIZE); //

	//	ret = down_killable(&cb->sem_write_able);
	//	DEBUG_LOG("RDMA WRITE local=%lld remote=%lld\n", local_offset, remote_offset);
		ret = ib_post_send(cb->qp, &rdma_sq_wr_copy, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post read err %d\n", ret);
			return ret;
		}
		ret = down_killable(&sem_write);
	//	up(&cb->sem_write_able);
		return 0;
	}
}

//NOT atomic, must check sem_read if it finish reading
static int do_read(struct krping_cb *cb,u64 local_offset,u64 remote_offset,u64 size){
	int ret;
	struct ib_send_wr *bad_wr;
	struct ib_send_wr rdma_sq_wr_copy= {0};	
	struct ib_sge rdma_sgl_new= {0};

	if(( local_offset%RPING_BUFSIZE+size>RPING_BUFSIZE)||
	   (remote_offset%RPING_BUFSIZE+size>RPING_BUFSIZE)){ //chk misalignment
		DEBUG_LOG("\nALERT, BUFFER MISALIGNMENT FOUND\n\n");
		return 1;
	}else if(remote_offset/RPING_BUFSIZE>cb->remote_pcount){
		DEBUG_LOG("\nALERT READ, %ld %ld %ld\n", local_offset, remote_offset, size);
		return 1;
	}else{
		rdma_sq_wr_copy.wr.rdma.rkey= cb->remote_rkey;
		rdma_sq_wr_copy.next 		= NULL;
		rdma_sq_wr_copy.send_flags 	= IB_SEND_SIGNALED;
		rdma_sq_wr_copy.num_sge 	= 1;

		rdma_sq_wr_copy.opcode	= IB_WR_RDMA_READ;
		rdma_sq_wr_copy.wr_id 	= (uint64_t)&cb->sem_read;
		rdma_sq_wr_copy.sg_list	= &rdma_sgl_new; 
		rdma_sgl_new.lkey 		= cb->dma_mr->rkey;

		rdma_sq_wr_copy.sg_list->length = size; //
		rdma_sgl_new.addr = cb->bigspace->dmapages[local_offset/RPING_BUFSIZE]+(local_offset%RPING_BUFSIZE); //
		rdma_sq_wr_copy.wr.rdma.remote_addr = cb->remote_addr[remote_offset/RPING_BUFSIZE]+(remote_offset%RPING_BUFSIZE); //

	//	ret = down_killable(&cb->sem_read_able);
	//	DEBUG_LOG("RDMA READ localoffset=%lld remoteoffset=%lld\n",local_offset,remote_offset);
		ret = ib_post_send(cb->qp, &rdma_sq_wr_copy, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post read err %d\n", ret);
			return ret;
		}
		ret = down_killable(&cb->sem_read);
	//	up(&cb->sem_read_able);
		return 0;
	}
}
// internal call,
static int do_read_bufferptr(struct krping_cb *cb)
{
	int ret;
	struct ib_send_wr *bad_wr;
	struct ib_send_wr rdma_sq_wr_copy= {0};	
	struct ib_sge rdma_sgl_new= {0};
	
	printk("theirptrs=%llx numpages=%d\n",cb->remote_dmabuf_ptr,cb->remote_pcount);
	rdma_sq_wr_copy.wr.rdma.rkey = cb->remote_rkey;
	rdma_sq_wr_copy.next = NULL;
	rdma_sq_wr_copy.send_flags = IB_SEND_SIGNALED; //no hardware ack needed
	rdma_sq_wr_copy.num_sge = 1;
    
	rdma_sq_wr_copy.opcode = IB_WR_RDMA_READ;
	rdma_sq_wr_copy.wr_id = (uint64_t)&cb->sem_read;
	rdma_sq_wr_copy.sg_list=&rdma_sgl_new; 
	rdma_sgl_new.lkey = cb->dma_mr->rkey;
  
	rdma_sgl_new.addr = (uint64_t)cb->remote_dmabuf_addr; //at offset 0
	rdma_sq_wr_copy.sg_list->length = sizeof(char*)*cb->remote_pcount; //
	rdma_sq_wr_copy.wr.rdma.remote_addr =cb->remote_dmabuf_ptr; // remote_addr

    
	ret = down_killable(&cb->sem_read_able);
	ret = ib_post_send(cb->qp, &rdma_sq_wr_copy, &bad_wr);
	if (ret) {

		printk(KERN_ERR PFX "post read err %d\n", ret);
		return ret;
	}

	ret = down_killable(&cb->sem_read);
	up(&cb->sem_read_able);
	cb->state=RDMA_READY; ///////// set this state here?
	return 0;
}

static int universal_send(struct krping_cb *cb, u64 imm, char* addr, u64 size){
	int ret;
	struct ib_send_wr *bad_wr;
	int slot;
	
	ret = down_killable(&cb->sem_verb_mutex);
	slot = cb->vslotusing;
	cb->vslotusing = (cb->vslotusing+1)%VERB_SEND_SLOT;
	up(&cb->sem_verb_mutex); //if not up here, it's up in event_handler

	cb->send_sgl[slot].length=size;
	memcpy(&cb->send_buf[slot],addr,size);

	cb->sq_wr[slot].ex.imm_data=imm; 

	//DEBUG_LOG("sending %s",&cb->send_buf[slot]);

	ret = ib_post_send(cb->qp, &cb->sq_wr[slot], &bad_wr);
	if(ret){
		DEBUG_LOG("SEND VERB ISSUE ERROR\n");

	}

	if(cb->sq_wr[slot].send_flags&IB_SEND_SIGNALED){
//		DEBUG_LOG("SEND VERB slot n-1, wait for ack\n");
		ret=down_killable(&cb->sem_verb_ack);
	}

	return 0;
}

//no more one send, then tell RDMA write whole things
static int send_buffer_info(struct krping_cb *cb)
{

	int ret,slot; //i
	struct buffer_info *info;
	struct ib_send_wr *bad_wr;
	ssleep(5);
  
	ret = down_killable(&cb->sem_verb_mutex);
		slot = cb->vslotusing;
		cb->vslotusing=(cb->vslotusing+1)%VERB_SEND_SLOT;
	up(&cb->sem_verb_mutex);
  
	info = (struct buffer_info *) &(cb->send_buf[slot]);
	DEBUG_LOG("about to send buffer info\n");
  
	//info->buf = htonll(cb->rdma_dma_addr);
	info->buf  = htonll(cb->ptable_dma_addr);
	info->rkey = htonl(cb->dma_mr->rkey); // cb->rdma_mr->rkey; // change!
	info->size = htonl(CONF_localpagecount);
	info->instanceno = htonl(cb->mynodeID);
	DEBUG_LOG("send RDMA buffer table addr %llx rkey %x len =%d pages\n", (uint64_t)cb->ptable_dma_addr, cb->dma_mr->rkey, CONF_localpagecount);
	
	cb->sq_wr[slot].ex.imm_data = 2; 
	cb->send_sgl[slot].length = sizeof(struct buffer_info);
	ret = ib_post_send(cb->qp, &cb->sq_wr[slot], &bad_wr);
	
	if(ret){
		DEBUG_LOG("SEND VERB ISSUE ERROR\n");
	}
	DEBUG_LOG("buffer info sent, wait for recv\n");

	//down for recv?
	wait_event_interruptible(cb->sem, cb->state >= RECV_INFO); 
	//do stuff needed once we know remote size
	cb->remote_addr = kmalloc(sizeof(char*)*cb->remote_pcount,GFP_KERNEL);
	if(!cb->remote_addr){
		DEBUG_LOG("kmalloc error\n");
	}
	cb->remote_dmabuf_addr = dma_map_single(cb->pd->device->dma_device,cb->remote_addr,sizeof(char*)*cb->remote_pcount, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, dmabuf_mapping, cb->remote_dmabuf_addr);
	//
	
	ret = do_read_bufferptr(cb);
	if(ret){
		DEBUG_LOG("%d:BUG IN READ THEIR PTR TABLE\n",cb->cbindex);
	} 
  //

  //wait_event_interruptible(cb->sem, cb->state >= RDMA_READY); //send done
  DEBUG_LOG("exchange buffer info success\n");

  return 0;
}

static void universal_recv_handler(struct krping_cb *cb, uint64_t imm, uint64_t slot)
{
	int i, ret;
	uint64_t bad_wr;
	//removed saved_buff, need &cb->recv_buf[slot] instead
	//DEBUG_LOG("%d: recv slot:%d\n",cb->cbindex,slot);

	switch(imm){
		case 2: //set buffers info
			cb->remote_pcount  = ntohl(cb->recv_buf[slot].buffer_info.size); //checkback, need hll? , do we really send big data
			cb->remote_rkey = ntohl(cb->recv_buf[slot].buffer_info.rkey);       
			cb->remotenodeID = ntohl(cb->recv_buf[slot].buffer_info.instanceno);
			cb->remote_dmabuf_ptr = ntohll(cb->recv_buf[slot].buffer_info.buf);
			DEBUG_LOG("otherside: id=%d rkey=%x ",cb->remotenodeID,cb->remote_rkey );
			
			cb->state=RECV_INFO;
			wake_up_interruptible(&cb->sem);

			break;
		case 4:
			DEBUG_LOG("up verb slots\n");
			break;
		case 5:
			DEBUG_LOG("recv exit\n");
			break;
		case 99: //set buffers info
			printk("unexpected,unhandled immediate received=%lld %s\n",imm,cb->recv_buf[slot].piggy);
			break;
		default:
			//DEBUG_LOG("bug default recv\n");
			COMEX_do_verb( imm, cb, slot); // COMEX
			break;
	}			

	ret = ib_post_recv(cb->qp, &cb->rq_wr[slot], &bad_wr);
	if(ret){
		printk(KERN_ERR PFX "post recv error: %d\n", ret);

	}
	return ;
}

static void krping_cq_event_handler_send(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
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
				printk(KERN_ERR PFX "send cq completion failed with "
					   "wr_id %Lx status %d opcode %d vender_err %x | Pair %d\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err, cb->remotenodeID);
				if(wc.status==13){
					DEBUG_LOG("need to retry\n");
          goto error;
				}else{
					goto error;
				}

			}
		}
		
		switch (wc.opcode) {
			case IB_WC_SEND:
				//DEBUG_LOG("send completion wc.wr_id=%d\n",wc.wr_id);
	
				//if(wc.wr_id%4==3){ //already plus one from alignment
					//DEBUG_LOG("unlock ");
					//up(&cb->sem_verb_mutex); 
					up(&cb->sem_verb_ack); //send one by one for now, simple and slow baseline version first 
				//}

				break;

			case IB_WC_RDMA_WRITE: // let it fall through
				//DEBUG_LOG("rdma write wr.id=%lld completion \n",wc.wr_id);
				
			case IB_WC_RDMA_READ:
				//DEBUG_LOG("rdma read completion\n");
				if(wc.wr_id!=0){
					up((struct semaphore*)wc.wr_id);
				}
				//up(&cb->sem_read);
				break;
			default:
				printk(KERN_ERR PFX "%s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode);
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
	int ret,i;
	int breakingout=0;

	BUG_ON(cb->cq_recv != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}

	ib_req_notify_cq(cb->cq_recv, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq_recv, 1, &wc)) == 1)
	{
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "recv cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
			case IB_WC_RECV:
/*				DEBUG_LOG("repost slot=%lld\n",wc.wr_id); //repost more receiving slot, quick!
				ret = ib_post_recv(cb->qp, &cb->rq_wr[(wc.wr_id+VERB_RECV_SLOT-1)%VERB_RECV_SLOT], &bad_wr); //toggle between buffer slot
				if (ret) {
					printk(KERN_ERR PFX "post recv error: %d\n", ret);
				}
*/
				if(wc.wc_flags & IB_WC_WITH_IMM){   
					universal_recv_handler(cb,wc.ex.imm_data,wc.wr_id);

				}else{
					printk("call recv handler but no imm\n");
				}
				//DEBUG_LOG("repost slot=%lld\n",wc.wr_id); //repost more receiving slot, quick!
				break;
			case IB_WC_RECV_RDMA_WITH_IMM:
				DEBUG_LOG("recv rdma with imm\n");
				break;
			default:
				printk(KERN_ERR PFX "%s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode);
			goto error;
		}
		
		if(breakingout){

			break;
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
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;

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
	int i;
	//DEBUG_LOG("CALLED krping_setup_wr\n");
	for(i=0;i<VERB_RECV_SLOT;i++){
		cb->recv_sgl[i].length = sizeof(union bufferx);
		cb->recv_sgl[i].lkey = cb->dma_mr->lkey; // cb->recv_mr->lkey; //
		cb->rq_wr[i].sg_list = &cb->recv_sgl[i];
		cb->rq_wr[i].num_sge = 1;
		cb->recv_sgl[i].addr = cb->recv_dma_addr+(i*(sizeof(union bufferx)) );
		cb->rq_wr[i].wr_id=i;

	//// work queue associated to each VRS



	}

	//send structures that have never changed, thus init here only once
	for(i=0;i<VERB_SEND_SLOT;i++){
		cb->sq_wr[i].opcode  = IB_WR_SEND_WITH_IMM;
		cb->sq_wr[i].send_flags =  IB_SEND_INLINE ; // IB_SEND_INLINE?, IB_SEND_SIGNALED?
		cb->sq_wr[i].num_sge = 1;

		cb->sq_wr[i].sg_list = &cb->send_sgl[i];   
		cb->sq_wr[i].wr_id   = i+1;

		cb->send_sgl[i].addr = cb->send_dma_addr+(sizeof(union bufferx)*i);
		cb->send_sgl[i].lkey = cb->dma_mr->lkey; //cb->send_mr->lkey; //
		cb->send_sgl[i].addr = (uint64_t)&cb->send_buf[i];
		if(i%4 == 0){ //the +1
			cb->sq_wr[i].send_flags |= IB_SEND_SIGNALED; //last slot is fenced (+1 from above)
		}
	}


  DEBUG_LOG("setup_wr done\n");
}

static int krping_setup_buffers(struct krping_cb *cb)
{
	int ret,i;
	int tests;
  
	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device, cb->recv_buf, sizeof(union bufferx)*VERB_RECV_SLOT, DMA_FROM_DEVICE); //recv in buffer, no edit to them
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
  
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,cb->send_buf,sizeof(union bufferx)*VERB_SEND_SLOT, DMA_TO_DEVICE); //send out buffer, no read back
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
  
	//set dma mask to 64, so more mem can be registerd
	if(!dma_set_mask(cb->pd->device->dma_device, 64)){
		DEBUG_LOG("mask NOT accepted!\n");
	}

	// MAP ONLY ONCE, what if i have multiple card?
	if(sg_dma_address(&cb->bigspace->sg[0])==0){
		DEBUG_LOG("\n the first one, need to map sg \n");
		tests=dma_map_sg(cb->pd->device->dma_device,cb->bigspace->sg,cb->bigspace->numbigpages,DMA_BIDIRECTIONAL);
		//init buffer addr to read
    
		for(i=0;i<cb->bigspace->numbigpages;i++){
			cb->bigspace->dmapages[i]=sg_dma_address(&cb->bigspace->sg[i]);
		}

	}
  
	/*
	DEBUG_LOG("some of my addr\n");
	for(i=0;i<100;i++){
		DEBUG_LOG("sg_dma_map=%llx regular addr =%lx sg_dma_len=%u\n",sg_dma_address(&(cb->sg[i])),cb->sg[i].page_link,sg_dma_len(&(cb->sg[i])));
	}
	*/
	DEBUG_LOG("done rdma malloc\n");

//page table ptr, to read ptr from, will initialize in dma_buffer send;
	cb->ptable_dma_addr = dma_map_single(cb->pd->device->dma_device,cb->bigspace->dmapages,sizeof(char*)*CONF_localpagecount, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, ptable_mapping, cb->ptable_dma_addr);
/* //move to when we know the size
	cb->remote_dmabuf_addr=dma_map_single(cb->pd->device->dma_device,&cb->remote_addr,sizeof(char*)*MAXPAGESCOUNT, DMA_BIDIRECTIONAL);
	pci_unmap_addr_set(cb, dmabuf_mapping, cb->remote_dmabuf_addr);
*/
  
	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
bail:
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
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);
	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->send_mr)
		ib_dereg_mr(cb->send_mr);
	if (cb->recv_mr)
		ib_dereg_mr(cb->recv_mr);

	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, recv_mapping),
			 sizeof(union bufferx)*VERB_RECV_SLOT, 
			 DMA_FROM_DEVICE);

	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, send_mapping),
			 sizeof(union bufferx)*VERB_SEND_SLOT, 
			 DMA_TO_DEVICE);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, ptable_mapping),
			 sizeof(char*)*CONF_localpagecount,
			 DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, dmabuf_mapping),
			 sizeof(char*)*cb->remote_pcount,
			 DMA_BIDIRECTIONAL);
			 
	kfree(cb->rdma_buf);


}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 128; //modified
	init_attr.cap.max_recv_sge = 8;
	init_attr.cap.max_send_sge = 8; 
	init_attr.cap.max_inline_data = 180; //MAX_INLINE_PAYLOAD+2 error...
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq_send;
	init_attr.recv_cq = cb->cq_recv;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR; //=0, means i need to request signal

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

	cb->cq_recv = ib_create_cq(cm_id->device, krping_cq_event_handler_recv, NULL, cb, cb->txdepth , 0);

	cb->cq_send = ib_create_cq(cm_id->device, krping_cq_event_handler_send, NULL, cb, cb->txdepth , 0);          

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
  
	//exchange buffer info
	ret = send_buffer_info(cb);
	if (ret) {
		printk(KERN_ERR PFX "%d:buffer info err %d\n",cb->cbindex, ret);
		return;
	}

	up(&cb->sem_ready);
	DEBUG_LOG("%d:unlocked from server\n",cb->cbindex);
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

static void krping_test_client(struct krping_cb *cb)
{
  
	int ret; //,start;
	//char t[200]="zxg";
	//start = 65;
  
	//exchange buffer info
	//sprintf(cb->bigspace->bufferpages[0],"rdma-ping-%d: ", 1); //someone will read here
	//DEBUG_LOG("%d:rdma buffer= %s\n",cb->cbindex,(char*)(cb->bigspace->bufferpages[0]) );

	//dma_sync_sg_for_device(cb->pd->device->dma_device, cb->sg, PAGESCOUNT, DMA_BIDIRECTIONAL);
	ret = send_buffer_info(cb); 
	if (ret) {
		printk(KERN_ERR PFX "buffer info error %d\n", ret);
		return;
	}
	up(&cb->sem_ready);
	DEBUG_LOG("%d:unlocked from client\n",cb->cbindex);
	//dma_sync_sg_for_cpu(cb->pd->device->dma_device, cb->sg, PAGESCOUNT, DMA_BIDIRECTIONAL);
	//printk("%d:string= %s\n",cb->cbindex,(char*)(cb->bigspace->bufferpages[0]+16) ); //someone write here //testbug

}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;
	conn_param.rnr_retry_count = 7;
 
	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	printk("%d: going to connect now\n",cb->cbindex); //bug here if server is not up
	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("%d: rdma_connect successful\n",cb->cbindex);
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

		printk(KERN_ERR PFX "addr/route resolution did not resolve: state %d\n", cb->state);

		return -EINTR;
	}


	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

void disconnect_cb(struct krping_cb *cb){
	if(cb->server){
		rdma_disconnect(cb->child_cm_id);
	}else{ //client
		rdma_disconnect(cb->cm_id);
	}
}

void winddown_cb(struct krping_cb *cb,int startingpoint)
{
	switch(startingpoint){
		case 0: //disconnect
			krping_free_buffers(cb); //fall through
		case 1: //free queuepair
			krping_free_qp(cb); //fall through
		case 2:
			if(cb->server){ 
				rdma_destroy_id(cb->child_cm_id);
			}

			//everyone
			DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
			rdma_destroy_id(cb->cm_id);
	}
}

static void krping_run_all(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret,i;
	
	if(cb->server){
		ret = krping_bind_server(cb); 
	}else{
		ret = krping_bind_client(cb);
	}  

	if (ret)
		return;

	if(cb->server){
		ret = krping_setup_qp(cb, cb->child_cm_id);
	}else{
		ret = krping_setup_qp(cb, cb->cm_id); 
	}

	cb->exitstatus=1;
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb); 
	cb->exitstatus=0;
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		return;
	}
	
	for(i=0; i<VERB_RECV_SLOT; i++){
		//DEBUG_LOG("%d:registering recv buffer %d\n",cb->cbindex,i);
		ret = ib_post_recv(cb->qp, &cb->rq_wr[i], &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
			return;
		}
	}

	if(cb->server){
		ret = krping_accept(cb);
	}else{
		//err in here if server doesn't exist
		ret = krping_connect_client(cb);
	}

	if(ret){
		printk(KERN_ERR PFX "connect error %d\n", ret);
		disconnect_cb(cb);
		return;
	}
	
	if(cb->server){
		krping_test_server(cb);
	}else{
		krping_test_client(cb);
	}
}

int krping_doit(char *cmd)
{

	struct task_struct *task[CONF_totalCB];
	struct krping_sharedspace *bigspaceptr;

	int op,i;
	int ret = 0;
	int totalcb = CONF_totalCB;
	char *optarg;
	unsigned long optint;
	struct semaphore sem_killsw;
	char stri[] = "responsethread  ";
	
	// for debug
	int j;
	char t[170] = "zxg   ";
	
	sema_init(&sem_killsw,0);
	bigspaceptr = kzalloc(sizeof(struct krping_sharedspace), GFP_KERNEL);
	if (!bigspaceptr)
		return -ENOMEM;
	regis_bigspace(bigspaceptr, CONF_localpagecount); //4MB each
	for(i=0; i<CONF_localpagecount; i++){
		printk(KERN_INFO "%s: Kmalloc -> %p\n", __FUNCTION__, bigspaceptr->bufferpages[i]);
	}

	cbs = kmalloc(sizeof(void*)*CONF_totalCB,GFP_KERNEL);
	for(i=0;i<totalcb;i++)
	{
		task[i] = (struct task_struct*)kzalloc(sizeof(struct task_struct), GFP_KERNEL);
		cbs[i]  = (struct krping_cb *)kzalloc(sizeof(struct krping_cb), GFP_KERNEL);
    
		if(!cbs[i]){
			return -ENOMEM;
		}
		
		cbs[i]->cbindex=i;
		cbs[i]->bigspace=bigspaceptr;
		cbs[i]->exitstatus=4;

		cbs[i]->state = IDLE;
		cbs[i]->size = RPING_BUFSIZE;
		cbs[i]->txdepth = RPING_SQ_DEPTH;

		init_waitqueue_head(&cbs[i]->sem);
		sema_init(&cbs[i]->sem_signal_ack,0);
		sema_init(&cbs[i]->sem_verb_ack, 0);
		sema_init(&cbs[i]->sem_verb_mutex, 1);
		sema_init(&cbs[i]->sem_read_able, 1);
		sema_init(&cbs[i]->sem_read, 0);
		sema_init(&cbs[i]->sem_write_able, 5);
		sema_init(&cbs[i]->sem_ready, 0);
		
		// IP
		cbs[i]->addr_str = CONF_allIP[i];
		in4_pton(CONF_allIP[i], -1, cbs[i]->addr, -1, NULL);
		cbs[i]->addr_type = AF_INET;
		DEBUG_LOG("ipaddr %d: (%s)\n", i,CONF_allIP[i]);
		
		// Port
		cbs[i]->port = CONF_allPort[i];
		DEBUG_LOG("port %d\n", (int)CONF_allPort[i]);
		
		// Node ID
		cbs[i]->mynodeID = CONF_nodeID;
		
		// server?
		cbs[i]->server = CONF_isServer[i];
		if(cbs[i]->server){
			DEBUG_LOG("server\n");
		}else{
			DEBUG_LOG("client\n");
		}
		DEBUG_LOG("=======\n");
	}
	
//// ???? check later
	mutex_lock(&krping_mutex);
	list_add_tail(&cbs[0]->list, &krping_cbs);
	mutex_unlock(&krping_mutex);
////

	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg, &optint)) != 0)
	{
		switch(op)
		{
			case 'a':
				cbs[0]->addr_str = optarg;
				in4_pton(optarg, -1, cbs[0]->addr, -1, NULL);
				cbs[0]->addr_type = AF_INET;
				DEBUG_LOG("ipaddr (%s)\n", optarg);
				break;
			case 'A':
				cbs[0]->addr_str = optarg;
				in6_pton(optarg, -1, cbs[0]->addr, -1, NULL);
				cbs[0]->addr_type = AF_INET6;
				DEBUG_LOG("ipv6addr (%s)\n", optarg);
				break;
			case 'p':
				cbs[0]->port = htons(optint);
				DEBUG_LOG("port %d\n", (int)optint);
				break;
			case 's':
				cbs[0]->server = 1;

				DEBUG_LOG("server\n");
				break;
			case 'c':
				cbs[0]->server = 0;

				DEBUG_LOG("client\n");
				break;
			case 'v':
				cbs[0]->verbose++;
				DEBUG_LOG("verbose\n");
				break;
		   //COMEX
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
		 //


			default:
				printk(KERN_ERR PFX "unknown opt %s\n", optarg);
				ret = -EINVAL;
				break;
		}
	}

	for(i=0; i<totalcb; i++){
		cbs[i]->cm_id = rdma_create_id(krping_cma_event_handler, cbs[i], RDMA_PS_TCP, IB_QPT_RC);
		if (IS_ERR(cbs[i]->cm_id)) {
			ret = PTR_ERR(cbs[i]->cm_id);
			printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
			goto out;
		}
	}
/////////////

	for(i=0; i<totalcb; i++){
		if(cbs[i]->server != 0){
			stri[14] = '0'+i;
			printk("server thread %d start\n",i);
			task[i] = kthread_run(&krping_run_all, (struct krping_cb *)cbs[i], stri);
		}
	}
	printk("going to sleep\n\n"); //5 sec before any client start
	ssleep(10);


//run all server first, then all client, just in case
	for(i=0; i<totalcb; i++){
		if(cbs[i]->server == 0){
			stri[14] = '0'+i;
			printk("client thread %d start\n", i);
			task[i] = kthread_run(&krping_run_all, (struct krping_cb *)cbs[i], stri);
		}
	}
	
//// chk for readiness of each
	for(i=0; i<totalcb; i++){
		ret = down_interruptible(&cbs[i]->sem_ready);
	}
	ssleep(5);

	DEBUG_LOG("Allthread ready to operate\n");
	DEBUG_LOG("===========================\n");
  //verb test
 
/*
	for(i=0;i<totalcb;i++){
		for(j=0;j<511;j++){
			//DEBUG_LOG("sending %d %d\n",i,j);
			sprintf(t,"zxyf %d %d",cbs[i]->cbindex,j);
			DEBUG_LOG("sending out %d\n",j);
			CHK(universal_send(cbs[i], 99,t, 14)) 
		}

    }
*/

//// ready to operate!  
	COMEX_init();	// for COMEX
	ret = down_interruptible(&sem_killsw); //never get unlocked naturally
	
//// should never run below this line
	DEBUG_LOG("\n\nKILL SWITCH UNLOCKED\n");
  
  
//// wind down process
	DEBUG_LOG("breaking down!\n");
	for(i=0; i<totalcb; i++){
		disconnect_cb(cbs[i]);
	}
	
//free buffer and deregister shared space
	dma_unmap_sg(cbs[0]->pd->device->dma_device,
				 cbs[0]->bigspace->sg,
				 cbs[0]->bigspace->numbigpages, DMA_BIDIRECTIONAL);

	for(i=0; i<CONF_localpagecount; i++){
		kfree((void*)cbs[0]->bigspace->bufferpages[i]);
	}
	
//decompose all buffers in cb
	for(i=0; i<totalcb; i++){
		winddown_cb(cbs[i],cbs[i]->exitstatus);


	}
	
out:
	mutex_lock(&krping_mutex);
	list_del(&cbs[0]->list);
	mutex_unlock(&krping_mutex);
	
	for(i=0; i<totalcb; i++){
		kfree(cbs[i]);
	}
	kfree(cbs);
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
module_exit(krping_exit); //need to be removed?