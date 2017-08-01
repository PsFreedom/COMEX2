#define MAX_TRY 5
#define MAX_MSSG 8

typedef struct{
	int mssg_qouta;
	int total_group;
	int back_off;

	spinlock_t list_lock;
	struct list_head free_group;
} COMEX_R_free_group_t;
COMEX_R_free_group_t *COMEX_free_group;

typedef struct{
	unsigned long addr_start;
	unsigned long addr_end;
	struct list_head link;
} free_group_t;

typedef struct{
	int head;
	int tail;
} buffer_desc_t;
buffer_desc_t *COMEX_writeOut_desc;

unsigned long COMEX_freelist_getpage(int);

////////////////////

void COMEX_freelist_print(int nodeID){
	free_group_t *myPtr;
	printk(KERN_INFO "List NO - %d\n", nodeID);
	list_for_each_entry(myPtr, &COMEX_free_group[nodeID].free_group, link){
		printk(KERN_INFO " >>> %lu - %lu\n", myPtr->addr_start, myPtr->addr_end);
	}
}

int COMEX_hash(int seed){
	return (seed+1)%COMEX_total_nodes;
}

uint64_t get_writeOut_buff(int node_ID, int buff_slot){
	uint64_t address;

	address  = COMEX_total_pages;
	address += node_ID*COMEX_writeOut;
	address += buff_slot;
	address *= X86PageSize;
	return address;
}

uint64_t get_readIn_buff(int buff_slot){
	uint64_t address;

	address  = COMEX_total_pages + (COMEX_writeOut*COMEX_total_nodes);
	address += buff_slot;
	address *= X86PageSize;
	return address;
}
////////////////////

int COMEX_move_to_Remote(struct page *old_page, int *retNodeID, unsigned long *retOffset)
{
	COMEX_address_t addr_struct;
	char *old_vAddr, *buf_vAddr;
	int i, dest_node = COMEX_hash(get_page_PID(old_page));
	
	spin_lock(&COMEX_free_group[dest_node].list_lock);
	for(i=0; i<MAX_TRY; i++){
		if(COMEX_free_group[dest_node].total_group < MAX_MSSG && 
		   COMEX_free_group[dest_node].mssg_qouta  > 0)
		{
			if(COMEX_free_group[dest_node].back_off <= 0){
				COMEX_free_group[dest_node].mssg_qouta--;
				COMEX_free_group[dest_node].back_off += 1<<(12 - COMEX_free_group[dest_node].mssg_qouta);
				
				COMEX_RDMA(dest_node, CODE_COMEX_PAGE_RQST, &COMEX_ID, sizeof(COMEX_ID));
			}
			else{
				COMEX_free_group[dest_node].back_off--;
			}
		}
		
		if(COMEX_free_group[dest_node].total_group > 0)
		{
			*retOffset = COMEX_freelist_getpage(dest_node);
			buf_vAddr  = (char *)get_writeOut_buff(0, 0);
			old_vAddr  = (char *)kmap(old_page);
			
			memcpy(COMEX_offset_to_addr(buf_vAddr), old_vAddr, X86PageSize);
			kunmap(old_page);
			
			addr_struct.local  = (unsigned long)buf_vAddr;
			addr_struct.remote = (unsigned long)*retOffset;
			addr_struct.size   = 1*X86PageSize;
			COMEX_RDMA(dest_node, CODE_COMEX_PAGE_WRTE, &addr_struct, sizeof(addr_struct));
			
			printk(KERN_INFO "WRITE: %lu - %lu %lu\n", *retOffset, checkSum_page(old_page), checkSum_Vpage(COMEX_offset_to_addr(buf_vAddr)));
			
			spin_unlock(&COMEX_free_group[dest_node].list_lock);
			*retNodeID = dest_node;
			return 1;
		}
		dest_node = COMEX_hash(dest_node);
	}

	spin_unlock(&COMEX_free_group[dest_node].list_lock);
	*retNodeID = 0;
	*retOffset = 0;
	return -1;
}

void COMEX_read_from_remote(struct page *new_page, int node_ID, unsigned long remote_addr)
{
	COMEX_address_t addr_struct;
	char *buf_vAddr = (char *)get_readIn_buff(0);
	char *new_vAddr = (char *)kmap(new_page);
	
	addr_struct.local  = (unsigned long)buf_vAddr;
	addr_struct.remote = (unsigned long)remote_addr;
	addr_struct.size   = 1*X86PageSize;
	COMEX_RDMA(node_ID, CODE_COMEX_PAGE_READ, &addr_struct, sizeof(addr_struct));
	
	memcpy(new_vAddr, COMEX_offset_to_addr(buf_vAddr), X86PageSize);	
	kunmap(new_page);
	
	printk(KERN_INFO "READ: %lu - %lu %lu\n", remote_addr, checkSum_page(new_page), checkSum_Vpage(COMEX_offset_to_addr(buf_vAddr)));
}
EXPORT_SYMBOL(COMEX_read_from_remote);

void COMEX_page_receive(int nodeID, int pageNO, int group_size)
{	
	spin_lock(&COMEX_free_group[nodeID].list_lock);
	if(pageNO > 0){
		free_group_t *group_ptr = (free_group_t *)kzalloc(sizeof(free_group_t), GFP_KERNEL);
		
		group_ptr->addr_start = (unsigned long)pageNO*X86PageSize;
		group_ptr->addr_end   = group_ptr->addr_start + (X86PageSize*(1<<group_size)) - X86PageSize;
		INIT_LIST_HEAD(&group_ptr->link);

		list_add_tail(&group_ptr->link, &COMEX_free_group[nodeID].free_group);
		COMEX_free_group[nodeID].mssg_qouta++;
		COMEX_free_group[nodeID].total_group++;
//		printk(KERN_INFO "%s: >>> %d %d %d - %lu %lu (%d | %d)\n", __FUNCTION__, nodeID, pageNO, group_size, group_ptr->addr_start, group_ptr->addr_end, COMEX_free_group[nodeID].mssg_qouta, COMEX_free_group[nodeID].back_off);

	}
	else{
		COMEX_free_group[nodeID].mssg_qouta++;
		COMEX_free_group[nodeID].back_off += 50000;
//		printk(KERN_INFO "%s: >>> %d %d %d - (%d | %d)\n", __FUNCTION__, nodeID, pageNO, group_size, COMEX_free_group[nodeID].mssg_qouta, COMEX_free_group[nodeID].back_off);
	}
	spin_unlock(&COMEX_free_group[nodeID].list_lock);
}
EXPORT_SYMBOL(COMEX_page_receive);

unsigned long COMEX_freelist_getpage(int list_ID)
{
	free_group_t *myPtr;
	unsigned long ret_addr = 200;
	
	myPtr = list_entry(COMEX_free_group[list_ID].free_group.next, free_group_t, link);
	ret_addr = myPtr->addr_start;
	myPtr->addr_start += X86PageSize;
	
	if(myPtr->addr_start > myPtr->addr_end){
		list_del(&myPtr->link);
		COMEX_free_group[list_ID].total_group--;
		kfree(myPtr);
//		printk(KERN_INFO "%s: Cut Link - %lu\n", __FUNCTION__, myPtr->addr_end/X86PageSize);
	}
	return ret_addr;
}


void COMEX_pages_request(int target)
{
	int i = COMEX_MAX_ORDER-1;
	reply_pages_t myStruct;
	
	myStruct.src_node = COMEX_ID;
	myStruct.page_no  = COMEX_rmqueue_smallest(i);
	myStruct.size     = i;
	while(myStruct.page_no<0 && i>8){
		i--;
		myStruct.page_no  = COMEX_rmqueue_smallest(i);
		myStruct.size     = i;
	}
	COMEX_RDMA(target, CODE_COMEX_PAGE_RPLY, &myStruct, sizeof(myStruct));
}
EXPORT_SYMBOL(COMEX_pages_request);