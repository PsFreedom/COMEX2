#include <linux/kernel.h>		/* Needed for KERN_INFO */
#include <linux/init.h>			/* Needed for the macros */
#include <linux/mm.h>			/* Needed for COMEX additional function */

static char proc_name[100];
static int node_ID;
static int n_nodes;
static int total_pages;
static int writeOut_buff;
static int readIn_buff;

void COMEX_module_echo_fn(char *str){
	printk(KERN_INFO "%s: Echo! %s\n", __FUNCTION__, str);
}

void COMEX_init()
{
	char *COMEX_area;
	long total_mem, i;
	printk(KERN_INFO "COMEX Kernel module V.2 --> %s\n", proc_name);
	printk(KERN_INFO "ID %d n_nodes %d total_pages %d\n", node_ID, n_nodes, total_pages);
	printk(KERN_INFO "writeOut_buff %d readIn_buff %d\n", writeOut_buff, readIn_buff);
	
	COMEX_module_echo = &COMEX_module_echo_fn;
}