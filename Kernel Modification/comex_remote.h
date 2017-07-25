#define MAX_TRY 5
#define MAX_MSSG 8
#define MIN_SIZE_RQ 8
#define MIN_GROUP_SIZE 8

typedef struct{
	int mssg_qouta;
	int total_group;
	int back_off;
	struct list_head free_group;
} COMEX_R_free_group_t;
COMEX_R_free_group_t *COMEX_free_group;

typedef struct{
	unsigned long addr_start;
	unsigned long addr_end;
	struct list_head link;
} free_group_t;

////////////////////

void print_freelist(int nodeID)
{
	free_group_t *myPtr;
	
	printk(KERN_INFO "List NO - %d\n", nodeID);
	list_for_each_entry(myPtr, &COMEX_free_group[nodeID].free_group, link){
		printk(KERN_INFO " >>> %lu - %lu\n", myPtr->addr_start, myPtr->addr_end);
	}
}

int COMEX_hash(int seed){
	return (seed+1)%COMEX_total_nodes;
}

int COMEX_move_to_Remote(struct page *old_page, int *retNodeID, unsigned long *retOffset)
{
	int i, dest_node;
//	printk(KERN_INFO "%s... Begin\n", __FUNCTION__);
	
	dest_node = COMEX_hash(get_page_PID(old_page));
	down_killable(&COMEX_remote_MUTEX);
	
	for(i=0; i<MAX_TRY; i++){
		if(COMEX_free_group[dest_node].total_group < MIN_SIZE_RQ/2 && 
		   COMEX_free_group[dest_node].mssg_qouta  > 0)
		{
			if(COMEX_free_group[dest_node].back_off <= 0){
				COMEX_free_group[dest_node].mssg_qouta--;
				COMEX_free_group[dest_node].back_off += 1<<(MAX_MSSG - COMEX_free_group[dest_node].mssg_qouta);
				
				COMEX_verb_send(dest_node, CODE_COMEX_PAGE_RQST, &COMEX_ID, sizeof(COMEX_ID));
			}
			else{
				COMEX_free_group[dest_node].back_off--;
			}
		}
		if(COMEX_free_group[dest_node].total_group > 0)
		{
			printk(KERN_INFO "%s: Claim to %d\n", __FUNCTION__, dest_node);
		}
		dest_node = COMEX_hash(dest_node);
	}

	up(&COMEX_remote_MUTEX);
	*retNodeID = 0;
	*retOffset = 0;
	return -1;
}

void COMEX_pages_request(int target)
{
	int i = COMEX_MAX_ORDER-1;
	reply_pages_t myStruct;
	
	myStruct.src_node = COMEX_ID;
	myStruct.page_no  = COMEX_rmqueue_smallest(i);
	myStruct.size     = i;
	while(myStruct.page_no < 0 && i > MIN_GROUP_SIZE){
		i--;
		myStruct.page_no  = COMEX_rmqueue_smallest(i);
		myStruct.size     = i;
	}
	
	COMEX_verb_send(target, CODE_COMEX_PAGE_RPLY, &myStruct, sizeof(myStruct));
}
EXPORT_SYMBOL(COMEX_pages_request);

void COMEX_page_receive(int nodeID, int pageNO, int group_size)
{
	free_group_t *group_ptr = (free_group_t *)kzalloc(sizeof(free_group_t), GFP_ATOMIC);
	
	group_ptr->addr_start = (unsigned long)pageNO*X86PageSize;
	group_ptr->addr_end   = group_ptr->addr_start + (X86PageSize*(1<<group_size)) - X86PageSize;
	INIT_LIST_HEAD(&group_ptr->link);
	
	spin_lock(&COMEX_freelist_spin);
	list_add_tail(&group_ptr->link, &COMEX_free_group[nodeID].free_group);
	spin_unlock(&COMEX_freelist_spin);
	
	print_freelist(nodeID);
//	printk(KERN_INFO "%s: >>> %d - %lu %lu \n", __FUNCTION__, nodeID, group_ptr->addr_start, group_ptr->addr_end);
}
EXPORT_SYMBOL(COMEX_page_receive);
