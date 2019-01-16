#include <linux/kernel.h>		/* Needed for KERN_INFO */
#include <linux/init.h>			/* Needed for the macros */
#include <linux/mm.h>			/* Needed for COMEX additional function */

//#define Total_CHKSM 6291456
//unsigned long module_CHKSM[2][Total_CHKSM];

static char proc_name[100];
static int total_pages;
static int writeOut_buff;
static int readIn_buff;
u64 remote_shift_offset;

struct workqueue_struct *COMEX_wq;

static uint64_t translate_useraddr(struct krping_cb *, uint64_t);
static int universal_send(struct krping_cb *cb, u64 imm, char* addr, u64 size);
static int do_write(struct krping_cb *cb,u64 local_offset,u64 remote_offset,u64 size);
static int do_read(struct krping_cb *cb,u64 local_offset,u64 remote_offset,u64 size);
void COMEX_do_work(struct work_struct *work);

typedef struct{
	struct work_struct my_work_struct;
	int CMD_num;
	char *args;
} work_content;

void COMEX_module_echo_fn(char *str)
{
	printk(KERN_INFO "%s: Echo! %s\n", __FUNCTION__, str);
}

int ID_to_CB(int nodeID){
	int i;	
	for(i=0; i<CONF_totalCB; i++){
		if(cbs[i]->remotenodeID == nodeID){
			return i;
		}
	}
	return -1;
}

uint64_t COMEX_offset_to_addr_fn(uint64_t offset){
	if(offset >= ((uint64_t)CONF_localpagecount*1024*4096))
		printk(KERN_INFO "%s: Wrong addr %llu >= %llu\n", __FUNCTION__, offset, ((uint64_t)CONF_localpagecount*1024*4096));
	return translate_useraddr(cbs[0], offset);
}

void COMEX_RDMA_fn(int target, int CMD_num, void *ptr, int struct_size)
{
	if(CMD_num == CODE_COMEX_PAGE_RQST){
	//	printk(KERN_INFO "PAGE_RQST: %d %p %d | %d\n", target, ptr, struct_size, *(int*)ptr);
		CHK(universal_send(cbs[target], CMD_num, ptr, struct_size))
	}
	else if(CMD_num == CODE_COMEX_PAGE_RPLY){
		reply_pages_t *myStruct = ptr;
		printk(KERN_INFO "PAGE_RPLY: %d->%d | %d %d %d\n", target, ID_to_CB(target), myStruct->src_node, myStruct->page_no, myStruct->size);
		
		if(myStruct->page_no % (1 << myStruct->size) != 0){
			printk(KERN_INFO "PAGE_RPLY: Wrong Addr!\n");
		}
		
		CHK(universal_send(cbs[ID_to_CB(target)], CMD_num, ptr, struct_size))
	}
	else if(CMD_num == CODE_COMEX_PAGE_WRTE){
		COMEX_address_t *myStruct = ptr;
	//	printk(KERN_INFO "PAGE_WRTE: %d | L %lu R %lu Size %d\n", target, myStruct->local >> PAGE_SHIFT, myStruct->remote >> PAGE_SHIFT, myStruct->size);
		CHK(do_write(cbs[target], myStruct->local, myStruct->remote + remote_shift_offset, myStruct->size << PAGE_SHIFT))
		COMEX_free_buff(target, myStruct->bufIDX, myStruct->size);
	}
	else if(CMD_num == CODE_COMEX_PAGE_READ){
		COMEX_address_t *myStruct = ptr;
	//	printk(KERN_INFO "PAGE_READ: %d | L %lu R %lu Size %d\n", target, myStruct->local, myStruct->remote, myStruct->size);
		CHK(do_read(cbs[target],  myStruct->local, myStruct->remote + remote_shift_offset, myStruct->size << PAGE_SHIFT))
	}
	else if(CMD_num == CODE_COMEX_PAGE_FREE){
		CHK(universal_send(cbs[target], CMD_num, ptr, struct_size))
	}
	else if(CMD_num == CODE_COMEX_PAGE_CKSM){
		CHK(universal_send(cbs[target], CMD_num, ptr, struct_size))
	}
	else{
		printk(KERN_INFO "%s... called: ERROR Unknown CMD_num %d\n", __FUNCTION__, CMD_num);
	}
}

void COMEX_do_verb(int CMD_num, struct krping_cb *cb, uint64_t slot)
{
	void *piggy = &cb->recv_buf[slot];
	work_content *myWork_cont = kzalloc(sizeof(work_content), GFP_ATOMIC);
	
	if(myWork_cont == NULL)
		return;
	
	myWork_cont->CMD_num = CMD_num;
	switch(CMD_num){
		case CODE_COMEX_PAGE_RQST:
	//		printk(KERN_INFO "Received... PAGE_RQST\n");
			myWork_cont->args = kzalloc(sizeof(int), GFP_ATOMIC);
			if(myWork_cont->args == NULL)
				return;
			memcpy( myWork_cont->args, piggy, sizeof(int));
			break;
		case CODE_COMEX_PAGE_RPLY:
	//		printk(KERN_INFO "Received... PAGE_RPLY\n");
			myWork_cont->args = kzalloc(sizeof(reply_pages_t), GFP_ATOMIC);
			if(myWork_cont->args == NULL)
				return;
			memcpy( myWork_cont->args, piggy, sizeof(reply_pages_t));
			break;
		case CODE_COMEX_PAGE_CKSM:
			printk(KERN_INFO "PAGE_CKSM: %lu - %lu\n", *(unsigned long *)piggy, checkSum_Vpage((char *)COMEX_offset_to_addr_fn(*(unsigned long *)piggy)));
			return;
		case CODE_COMEX_PAGE_FREE:
			myWork_cont->args = kzalloc(sizeof(free_struct_t), GFP_ATOMIC);
			if(myWork_cont->args == NULL)
				return;
			memcpy( myWork_cont->args, piggy, sizeof(free_struct_t));
			break;
		default:
			printk(KERN_INFO "%s: %d | ERROR Unknown CMD_num %d\n", __FUNCTION__, *(int *)piggy, CMD_num);
			return;
	}
	INIT_WORK(&myWork_cont->my_work_struct, COMEX_do_work);
	queue_work(COMEX_wq, &myWork_cont->my_work_struct);
}

void COMEX_do_work(struct work_struct *work)
{
//	work_content *myWork_cont = (work_content *)container_of(work, work_content, my_work_struct);
	work_content *myWork_cont = (work_content *)work;
	int CMD_num = myWork_cont->CMD_num;
	void *piggy = myWork_cont->args;
	
	if(CMD_num == CODE_COMEX_PAGE_RQST){
//		printk(KERN_INFO "PAGE_RQST: From node %d\n", *(int *)piggy);
		COMEX_pages_request(*(int *)piggy);
        kfree((int *)piggy);
	}
	else if(CMD_num == CODE_COMEX_PAGE_RPLY){
		reply_pages_t *myStruct = piggy;
//		printk(KERN_INFO "PAGE_RPLY: %d->%d | %d %d %d\n", myStruct->src_node, ID_to_CB(myStruct->src_node), myStruct->src_node, myStruct->page_no, myStruct->size);
		COMEX_page_receive(ID_to_CB(myStruct->src_node), myStruct->page_no, myStruct->size);
        kfree(myStruct);
	}
	else if(CMD_num == CODE_COMEX_PAGE_FREE){
		int i, pow, page_idx;
		free_struct_t *myStruct = piggy;
/*		
		for(i=0; i<MAX_FREE; i++){
			while(myStruct->count[i] > 0){
				COMEX_free_page(myStruct->pageNO[i], 0);
				myStruct->pageNO[i]++;
				myStruct->count[i]--;
			}
		}
*/		
//		printk(KERN_INFO "PAGE_FREE: ##################\n");
		for(i=0; i<MAX_FREE; i++){
//			printk(" >>> %d %hd\n", myStruct->pageNO[i], myStruct->count[i]);
			while(myStruct->count[i] > 0)
			{
				pow		 = COMEX_MAX_ORDER-1;
				page_idx = myStruct->pageNO[i] & ((1 << (COMEX_MAX_ORDER-1)) - 1);
				while((1<<pow) > myStruct->count[i] || page_idx%(1<<pow) != 0){
					pow--;
				}
				COMEX_free_page(myStruct->pageNO[i], pow);
				myStruct->pageNO[i] += (1<<pow);
				myStruct->count[i]  -= (1<<pow);
//				printk(" ------ %d - %d | %d %hd\n", page_idx, (1<<pow), myStruct->pageNO[i], myStruct->count[i]);
			}
		}
        kfree(myStruct);
	}
	kfree(myWork_cont);
}

void COMEX_init(){	
	COMEX_module_echo    = &COMEX_module_echo_fn;
	COMEX_offset_to_addr = &COMEX_offset_to_addr_fn;
	COMEX_RDMA 			 = &COMEX_RDMA_fn;

	remote_shift_offset  = 0UL;
	remote_shift_offset += writeOut_buff*CONF_totalCB;
	remote_shift_offset += readIn_buff;
	remote_shift_offset  = remote_shift_offset << 12;
	
//	COMEX_wq = alloc_workqueue("COMEX WorkQueue", WQ_MEM_RECLAIM | WQ_NON_REENTRANT | WQ_HIGHPRI, 0);
	COMEX_wq = create_singlethread_workqueue("COMEX WorkQueue");
	COMEX_init_ENV(CONF_nodeID, CONF_totalCB, writeOut_buff, readIn_buff, total_pages, proc_name);
}