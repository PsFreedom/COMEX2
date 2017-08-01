#include <linux/kernel.h>		/* Needed for KERN_INFO */
#include <linux/init.h>			/* Needed for the macros */
#include <linux/mm.h>			/* Needed for COMEX additional function */

static char proc_name[100];
static int total_pages;
static int writeOut_buff;
static int readIn_buff;

static uint64_t translate_useraddr(struct krping_cb *, uint64_t);
static int universal_send(struct krping_cb *cb, u64 imm, char* addr, u64 size);
static void universal_queue_send(struct krping_cb *cb, u64 imm, char* addr, u64 size);

void COMEX_module_echo_fn(char *str)
{
	printk(KERN_INFO "%s: Echo! %s\n", __FUNCTION__, str);
}

int ID_to_CB(int nodeID)
{
	int i;	
	for(i=0; i<CONF_totalCB; i++){
		if(cbs[i]->remotenodeID == nodeID){
			return i;
		}
	}
	return -1;
}

uint64_t COMEX_offset_to_addr_fn(uint64_t offset)
{
	if(offset >= ((uint64_t)PAGESCOUNT*1024*4096))
		printk(KERN_INFO "%s: Wrong addr %llu >= %llu\n", __FUNCTION__, offset, ((uint64_t)PAGESCOUNT*1024*4096));
	return translate_useraddr(cbs[0], offset);
}

void COMEX_RDMA_fn(int target, int CMD_num, void *ptr, int struct_size)
{
	if(CMD_num == CODE_COMEX_PAGE_RQST){
//		printk(KERN_INFO "%s: %d %d %p %d | %d\n", __FUNCTION__, target, CMD_num, ptr, struct_size, *(int*)ptr);
		universal_queue_send(cbs[target], CMD_num, ptr, struct_size);
	}
	else if(CMD_num == CODE_COMEX_PAGE_RPLY){
		reply_pages_t *myStruct = ptr;
		printk(KERN_INFO "PAGE_RPLY: %d->%d %d | %d %d %d\n", target, ID_to_CB(target), CMD_num, myStruct->src_node, myStruct->page_no, myStruct->size);
		universal_queue_send(cbs[ID_to_CB(target)], CMD_num, ptr, struct_size);
	}
	else{
		printk(KERN_INFO "%s... called: ERROR Unknown CMD_num %d\n", __FUNCTION__, CMD_num);
	}
}

void COMEX_do_verb(int CMD_num, void *piggy)
{
	if(CMD_num == CODE_COMEX_PAGE_RQST){
//		printk(KERN_INFO "%s: %d %d\n", __FUNCTION__, CMD_num, *(int *)piggy);
		COMEX_pages_request(*(int *)piggy);
	}
	else if(CMD_num == CODE_COMEX_PAGE_RPLY){
		reply_pages_t *myStruct = piggy;
//		printk(KERN_INFO "%s: %d %p | %d %d %d\n", __FUNCTION__, CMD_num, piggy, myStruct->src_node, myStruct->page_no, myStruct->size);
		COMEX_page_receive(ID_to_CB(myStruct->src_node), myStruct->page_no, myStruct->size);
	}
	else{
		printk(KERN_INFO "%s: %d | ERROR Unknown CMD_num %d\n", __FUNCTION__, *(int *)piggy, CMD_num);
	}
}

void COMEX_init()
{	
	char test_str[11]="TEST 1234";
	COMEX_module_echo    = &COMEX_module_echo_fn;
	COMEX_offset_to_addr = &COMEX_offset_to_addr_fn;
	COMEX_RDMA 			 = &COMEX_RDMA_fn;
	
	COMEX_init_ENV(CONF_nodeID, CONF_totalCB, writeOut_buff, readIn_buff, total_pages, proc_name);
}
