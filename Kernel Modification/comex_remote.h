#define MAX_TRY 5
#define MAX_MSSG 8
#define FLUSH 32
#define PreF_BITS 4
#define PreF_SIZE (1UL << PreF_BITS)	// 0000 1000
#define PreF_MASK (~(PreF_SIZE - 1))	// 0000 0111 -> 1111 1000

typedef struct{
	int mssg_qouta;
	int total_group;
	int back_off;

//	spinlock_t list_lock;
	struct mutex mutex_FG;
	struct list_head free_group;
} COMEX_R_free_group_t;
COMEX_R_free_group_t *COMEX_free_group;

typedef struct{
	int page_start;
	int page_end;
	struct list_head link;
} free_group_t;

typedef struct{
	char status;
	int  nodeID;
	int  pageNO;
	struct mutex mutex_buff;
} buff_desc_t;
buff_desc_t **COMEX_writeOut_buff;
buff_desc_t *COMEX_readIn_buff;

typedef struct{
	short  head;
	short  tail;
	spinlock_t pos_lock;
} buff_pos_t;
buff_pos_t *buff_pos;

free_struct_t *COMEX_free_struct;

int COMEX_freelist_getpage(int list_ID);
void COMEX_free_to_remote(int nodeID, int pageNO);
void COMEX_flush_buff(int nodeID);
void COMEX_flush_one(int nodeID, int slot);
void invalidate_buffer(int nodeID, int pageNO);

////////////////////

void COMEX_freelist_print(int nodeID){
	free_group_t *myPtr;
	printk(KERN_INFO "List NO - %d\n", nodeID);
	list_for_each_entry(myPtr, &COMEX_free_group[nodeID].free_group, link){
		printk(KERN_INFO " >>> %d - %d\n", myPtr->page_start, myPtr->page_end);
	}
}

int COMEX_hash(int seed){
	return (seed+1)%COMEX_total_nodes;
}

uint64_t get_writeOut_buff(int node_ID, int buff_slot){
	uint64_t address;

//	address  = COMEX_total_pages;
	address  = 0;
	address += node_ID*COMEX_total_writeOut;
	address += buff_slot;
	address *= X86PageSize;
	return address;
}

uint64_t get_readIn_buff(int buff_slot){
	uint64_t address;

//	address  = COMEX_total_pages;
	address  = 0;
	address += COMEX_total_writeOut*COMEX_total_nodes;
	address += buff_slot;
	address *= X86PageSize;
	return address;
}
////////////////////

int COMEX_move_to_Remote(struct page *old_page, int *retNodeID, int *retPageNO)
{
	char *old_vAddr, *buf_vAddr;
	int i, buff_slot, dest_node = COMEX_hash(get_page_PID(old_page));
	
	mutex_lock(&mutex_PF);
	for(i=0; i<COMEX_total_nodes; i++)
	{
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
		if(COMEX_free_group[dest_node].total_group > 0 && 
		   COMEX_writeOut_buff[dest_node][buff_pos[dest_node].tail].status == -1)
		{
			*retPageNO = COMEX_freelist_getpage(dest_node);
			
			buff_slot = buff_pos[dest_node].tail;
			buff_pos[dest_node].tail++;
			buff_pos[dest_node].tail %= COMEX_total_writeOut;
			COMEX_writeOut_buff[dest_node][buff_slot].status = 1;
			
			buf_vAddr  = (char *)get_writeOut_buff(dest_node, buff_slot);
			old_vAddr  = (char *)kmap(old_page);
			memcpy((char *)COMEX_offset_to_addr((uint64_t)buf_vAddr), old_vAddr, X86PageSize);
			kunmap(old_page);
			
			COMEX_writeOut_buff[dest_node][buff_slot].nodeID = dest_node;
			COMEX_writeOut_buff[dest_node][buff_slot].pageNO = *retPageNO;
			COMEX_writeOut_buff[dest_node][buff_slot].status = 2;
//			if(buff_slot%FLUSH == 0 && buff_slot != 0)
//				COMEX_flush_buff(dest_node);

			COMEX_CHKSM[dest_node][*retPageNO].val 	   = checkSum_page(old_page);
			COMEX_CHKSM[dest_node][*retPageNO].counter = 0;
			COMEX_flush_one(dest_node, buff_slot);
			
			mutex_unlock(&mutex_PF);
			*retNodeID = dest_node;
			return 1;
		}
		dest_node = COMEX_hash(dest_node);
	}
	mutex_unlock(&mutex_PF);
	*retNodeID = 0;
	*retPageNO = 0;
	return -1;
}

void COMEX_read_from_remote(struct page *new_page, int node_ID, int pageNO)
{
	int buff_IDX = pageNO % COMEX_total_readIn;
	char *buf_vAddr = (char *)get_readIn_buff(buff_IDX);
	char *new_vAddr = (char *)kmap(new_page);
	COMEX_address_t addr_struct;

//	mutex_lock(&COMEX_readIn_buff[buff_IDX].mutex_buff);
	mutex_lock(&mutex_PF);
	addr_struct.local  = (unsigned long)buf_vAddr;
	addr_struct.remote = (unsigned long)pageNO << SHIFT_PAGE;
	addr_struct.size   = 1;
	addr_struct.bufIDX = buff_IDX;
	
	addr_struct.dstAddr= new_vAddr;
	addr_struct.srcAddr= (char *)COMEX_offset_to_addr((uint64_t)get_readIn_buff(buff_IDX));
	COMEX_RDMA(node_ID, CODE_COMEX_PAGE_READ, &addr_struct, sizeof(addr_struct));
//	mutex_unlock(&COMEX_readIn_buff[buff_IDX].mutex_buff);
	mutex_unlock(&mutex_PF);
	
//	memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)get_readIn_buff(buff_FLR)), X86PageSize);
	kunmap(new_page);
	COMEX_in_RDMA++;
}
EXPORT_SYMBOL(COMEX_read_from_remote);

void COMEX_page_receive(int nodeID, int pageNO, int group_size)
{
	mutex_lock(&mutex_PF);
	if(pageNO >= 0){
		free_group_t *group_ptr = (free_group_t *)kzalloc(sizeof(free_group_t), GFP_KERNEL);
		
		group_ptr->page_start = pageNO;
		group_ptr->page_end   = pageNO + (1UL<<group_size) - 1;
		INIT_LIST_HEAD(&group_ptr->link);

		list_add_tail(&group_ptr->link, &COMEX_free_group[nodeID].free_group);
		COMEX_free_group[nodeID].mssg_qouta++;
		COMEX_free_group[nodeID].total_group++;
	}
	else{
		COMEX_free_group[nodeID].mssg_qouta++;
		COMEX_free_group[nodeID].back_off += 50000;
	}
	mutex_unlock(&mutex_PF);
}
EXPORT_SYMBOL(COMEX_page_receive);

int COMEX_freelist_getpage(int list_ID)
{
	free_group_t *myPtr;
	int ret_pageNO = -1;
	
	myPtr      = list_entry(COMEX_free_group[list_ID].free_group.next, free_group_t, link);
	ret_pageNO = myPtr->page_start;
	myPtr->page_start++;
	if(myPtr->page_start > myPtr->page_end){
		list_del(&myPtr->link);
		COMEX_free_group[list_ID].total_group--;
		kfree(myPtr);
//		printk(KERN_INFO "%s: Cut Link - %lu\n", __FUNCTION__, myPtr->addr_end/X86PageSize);
	}
	return ret_pageNO;
}

void clean_free_struct(int nodeID){
	int i;
	for(i=0; i<MAX_FREE; i++){
		COMEX_free_struct[nodeID].pageNO[i] = -1;
		COMEX_free_struct[nodeID].count[i]  =  0;
	}
}

void COMEX_free_to_remote(int nodeID, int pageNO)
{
	int i;
	spin_lock(&freePage_spin);
	for(i=0; i<MAX_FREE; i++){
		if( COMEX_free_struct[nodeID].pageNO[i] + COMEX_free_struct[nodeID].count[i] == pageNO && COMEX_free_struct[nodeID].count[i] < 16384){
			COMEX_free_struct[nodeID].count[i]++;
			spin_unlock(&freePage_spin);
			return;
		}
		if( COMEX_free_struct[nodeID].pageNO[i] - 1 == pageNO && COMEX_free_struct[nodeID].count[i] < 16384){
			COMEX_free_struct[nodeID].pageNO[i]--;
			COMEX_free_struct[nodeID].count[i]++;
			spin_unlock(&freePage_spin);
			return;
		}
		if( COMEX_free_struct[nodeID].pageNO[i] == -1){
			COMEX_free_struct[nodeID].pageNO[i] = pageNO;
			COMEX_free_struct[nodeID].count[i]  = 1;
			spin_unlock(&freePage_spin);
			return;
		}
	}
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_FREE, &COMEX_free_struct[nodeID], sizeof(COMEX_free_struct[nodeID]));
	clean_free_struct(nodeID);
	COMEX_free_struct[nodeID].pageNO[0] = pageNO;
	COMEX_free_struct[nodeID].count[0]  = 1;
	spin_unlock(&freePage_spin);
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

void COMEX_free_buff(int nodeID, int pageNO, int con_page)
{
	while(con_page > 0){
//		printk(KERN_INFO "Free Buff: %d %d\n", nodeID, pageNO);
		COMEX_writeOut_buff[nodeID][pageNO].status = -1;
		COMEX_writeOut_buff[nodeID][pageNO].nodeID = -1;
		COMEX_writeOut_buff[nodeID][pageNO].pageNO = -1;
		con_page--;
		pageNO++;
	}
}
EXPORT_SYMBOL(COMEX_free_buff);

void COMEX_flush_one(int nodeID, int slot)
{
	COMEX_address_t addr_struct;
	
	addr_struct.local  = (unsigned long)get_writeOut_buff(nodeID, slot);
	addr_struct.remote = (unsigned long)COMEX_writeOut_buff[nodeID][slot].pageNO << SHIFT_PAGE;
	addr_struct.size   = 1;
	addr_struct.bufIDX = slot;
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_WRTE, &addr_struct, sizeof(addr_struct));
}
