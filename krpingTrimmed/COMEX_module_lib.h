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

void COMEX_module_echo_fn(char *str){
	printk(KERN_INFO "%s: Echo! %s\n", __FUNCTION__, str);
}

uint64_t COMEX_offset_to_addr_fn(uint64_t offset){
	return translate_useraddr(global_CB, offset);
}

void COMEX_init()
{	
	COMEX_module_echo    = &COMEX_module_echo_fn;
	COMEX_offset_to_addr = &COMEX_offset_to_addr_fn;
	
	COMEX_init_ENV(node_ID, n_nodes-1, writeOut_buff, readIn_buff, total_pages, proc_name);
}