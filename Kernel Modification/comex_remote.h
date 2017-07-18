typedef struct{
	int total_group;
	struct list_head free_group;
} COMEX_R_free_group_t;
COMEX_R_free_group_t *COMEX_R_free_group;

////////////////////

int COMEX_hash(int seed){
	return (seed+1)%COMEX_total_nodes;
}

int COMEX_move_to_Remote(struct page *old_page, int *retNodeID, unsigned long *retOffset)
{
	printk(KERN_INFO "%s... Begin\n", __FUNCTION__);
	return -1;
}
