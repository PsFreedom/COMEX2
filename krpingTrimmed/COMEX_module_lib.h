#include <linux/kernel.h>		/* Needed for KERN_INFO */
#include <linux/init.h>			/* Needed for the macros */
#include <linux/mm.h>			/* Needed for COMEX additional function */

static char proc_name[100];
static int node_ID;
static int n_nodes;
static int total_pages;
static int writeOut_buff;
static int readIn_buff;

struct krping_cb *global_CB;
static uint64_t translate_useraddr(struct krping_cb *, uint64_t);
static int universal_send(struct krping_cb *cb, u64 imm, char* addr, u64 size);

void COMEX_module_echo_fn(char *str){
	printk(KERN_INFO "%s: Echo! %s\n", __FUNCTION__, str);
}

uint64_t COMEX_offset_to_addr_fn(uint64_t offset)
{
	if(offset >= ((uint64_t)PAGESCOUNT*1024*4096))
		printk(KERN_INFO "%s: Wrong addr %llu >= %llu\n", __FUNCTION__, offset, ((uint64_t)PAGESCOUNT*1024*4096));
	return translate_useraddr(global_CB, offset);
}

void COMEX_verb_send_fn(int target, int CMD_num, void *ptr, int struct_size)
{
//	CHK(universal_send(global_CB, CMD_num, ptr, struct_size))
	if(CMD_num == 10010){
		printk(KERN_INFO "%s: %d %d %p %d | %d\n", __FUNCTION__, target, CMD_num, ptr, struct_size, *(int*)ptr);
	}
	else if(CMD_num == 10011){
		reply_pages_t *myStruct = ptr;
		printk(KERN_INFO "%s: %d %d %p %d | %d %d %d\n", __FUNCTION__, target, CMD_num, ptr, struct_size, myStruct->src_node, myStruct->page_no, myStruct->size);
	}
}

void COMEX_do_verb(int CMD_num, void *piggy)
{
	printk(KERN_INFO "%s: %d %p\n", __FUNCTION__, CMD_num, piggy);
}

void COMEX_init()
{	
	char test_str[11]="TEST 1234";

	COMEX_module_echo    = &COMEX_module_echo_fn;
	COMEX_offset_to_addr = &COMEX_offset_to_addr_fn;
	COMEX_verb_send 	 = &COMEX_verb_send_fn;
	
	COMEX_init_ENV(node_ID, n_nodes-1, writeOut_buff, readIn_buff, total_pages, proc_name);
	CHK(universal_send(global_CB, 99, test_str, 10))
	
	COMEX_pages_request(4);
	COMEX_pages_request(2);
	COMEX_pages_request(7);
	COMEX_pages_request(9);
	COMEX_pages_request(147);
}
