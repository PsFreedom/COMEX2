#define MAX_TRY 5;
#define MAX_MSSG 16;

typedef struct{
	int mssg_qouta;
	int total_group;
	struct list_head free_group;
} COMEX_R_free_group_t;
COMEX_R_free_group_t *COMEX_free_group;

typedef struct{
	unsigned long addr_start;
	unsigned long addr_end;
	struct list_head link;
} free_group_t;

////////////////////

int COMEX_hash(int seed){
	return (seed+1)%COMEX_total_nodes;
}

int COMEX_move_to_Remote(struct page *old_page, int *retNodeID, unsigned long *retOffset)
{
	int i, dest_node;
//	printk(KERN_INFO "%s... Begin\n", __FUNCTION__);
	
	dest_node = COMEX_hash(get_page_PID(old_page));

	return -1;
}
