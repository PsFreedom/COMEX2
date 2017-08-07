#define MAX_TRY 5
#define MAX_MSSG 8
#define FLUSH 32

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
	char status;
	int  nodeID;
	int  pageNO;
	unsigned long remote;
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

unsigned long COMEX_freelist_getpage(int);
void COMEX_free_to_remote(int nodeID, unsigned long offset);
void COMEX_flush_buff(int nodeID);
void COMEX_flush_one(int nodeID, int slot);

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
	address += node_ID*COMEX_total_writeOut;
	address += buff_slot;
	address *= X86PageSize;
	return address;
}

uint64_t get_readIn_buff(int buff_slot){
	uint64_t address;

	address  = COMEX_total_pages + (COMEX_total_writeOut*COMEX_total_nodes);
	address += buff_slot;
	address *= X86PageSize;
	return address;
}
////////////////////

int COMEX_move_to_Remote(struct page *old_page, int *retNodeID, unsigned long *retOffset)
{
	char *old_vAddr, *buf_vAddr;
	int i, buff_slot, dest_node = COMEX_hash(get_page_PID(old_page));
	
	for(i=0; i<MAX_TRY; i++)
	{
		spin_lock(&COMEX_free_group[dest_node].list_lock);
		if(COMEX_free_group[dest_node].total_group < MAX_MSSG && 
		   COMEX_free_group[dest_node].mssg_qouta  > 0)
		{
			if(COMEX_free_group[dest_node].back_off <= 0){
				COMEX_free_group[dest_node].mssg_qouta--;
				COMEX_free_group[dest_node].back_off += 1<<(12 - COMEX_free_group[dest_node].mssg_qouta);
				spin_unlock(&COMEX_free_group[dest_node].list_lock);
				
				COMEX_RDMA(dest_node, CODE_COMEX_PAGE_RQST, &COMEX_ID, sizeof(COMEX_ID));
				spin_lock(&COMEX_free_group[dest_node].list_lock);
			}
			else{
				COMEX_free_group[dest_node].back_off--;
			}
		}
		
		if(COMEX_free_group[dest_node].total_group > 0 && COMEX_writeOut_buff[dest_node][buff_pos[dest_node].tail].status == -1)
		{
			*retOffset = COMEX_freelist_getpage(dest_node);
			buf_vAddr  = (char *)get_writeOut_buff(dest_node, buff_pos[dest_node].tail);
			old_vAddr  = (char *)kmap(old_page);
			
			buff_slot = buff_pos[dest_node].tail;
			buff_pos[dest_node].tail++;
			buff_pos[dest_node].tail %= COMEX_total_writeOut;
			COMEX_writeOut_buff[dest_node][buff_slot].status = 1;
			spin_unlock(&COMEX_free_group[dest_node].list_lock);
			
			memcpy(COMEX_offset_to_addr(buf_vAddr), old_vAddr, X86PageSize);
			COMEX_writeOut_buff[dest_node][buff_slot].nodeID = dest_node;
			COMEX_writeOut_buff[dest_node][buff_slot].pageNO = (int)((*retOffset)/X86PageSize);
			COMEX_writeOut_buff[dest_node][buff_slot].status = 2;
			COMEX_writeOut_buff[dest_node][buff_slot].remote = *retOffset;
			kunmap(old_page);
			
			COMEX_flush_one(dest_node, buff_slot);
//			if(buff_slot%FLUSH == 0 && buff_slot != 0)
//				COMEX_flush_buff(dest_node);
			*retNodeID = dest_node;
			return 1;
		}
		spin_unlock(&COMEX_free_group[dest_node].list_lock);
		dest_node = COMEX_hash(dest_node);
	}
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
	COMEX_free_to_remote(node_ID, remote_addr);
	
	memcpy(new_vAddr, COMEX_offset_to_addr(buf_vAddr), X86PageSize);	
	kunmap(new_page);
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

void clean_free_struct(int nodeID){
	int i;
	for(i=0; i<MAX_FREE; i++){
		COMEX_free_struct[nodeID].pageNO[i] = -1;
		COMEX_free_struct[nodeID].count[i]  =  0;
	}
}

void COMEX_free_to_remote(int nodeID, unsigned long offset)
{
	int pageNO, i;
	offset = offset/X86PageSize;
	pageNO = (int)offset;
	
	for(i=0; i<MAX_FREE; i++){
		if( COMEX_free_struct[nodeID].pageNO[i] + COMEX_free_struct[nodeID].count[i] == pageNO && COMEX_free_struct[nodeID].count[i] < 16384){
			COMEX_free_struct[nodeID].count[i]++;
			return;
		}
		if( COMEX_free_struct[nodeID].pageNO[i] - 1 == pageNO && COMEX_free_struct[nodeID].count[i] < 16384){
			COMEX_free_struct[nodeID].pageNO[i]--;
			COMEX_free_struct[nodeID].count[i]++;
			return;
		}
		if( COMEX_free_struct[nodeID].pageNO[i] == -1){
			COMEX_free_struct[nodeID].pageNO[i] = pageNO;
			COMEX_free_struct[nodeID].count[i]  = 1;
			return;
		}
	}
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_FREE, &COMEX_free_struct[nodeID], sizeof(COMEX_free_struct[nodeID]));
	clean_free_struct(nodeID);
	COMEX_free_struct[nodeID].pageNO[0] = pageNO;
	COMEX_free_struct[nodeID].count[0]  = 1;
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

void COMEX_flush_buff(int nodeID)
{
	int count = 0;
	COMEX_address_t addr_struct;
	spin_lock(&buff_pos[nodeID].pos_lock);
	
	while(COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].status == 2){
		COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].status = 3;
		count++;
		
		if(buff_pos[nodeID].head + count == COMEX_total_writeOut)
			break;
	}
	
	addr_struct.local  = (unsigned long)get_writeOut_buff(nodeID, buff_pos[nodeID].head);
	addr_struct.remote = (unsigned long)COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head].remote;
	addr_struct.size   = count * X86PageSize;
	addr_struct.bufIDX = buff_pos[nodeID].head;
	buff_pos[nodeID].head += count;
	buff_pos[nodeID].head %= COMEX_total_writeOut;
	spin_unlock(&buff_pos[nodeID].pos_lock);
	
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_WRTE, &addr_struct, sizeof(addr_struct));
}

void COMEX_flush_one(int nodeID, int slot)
{
	COMEX_address_t addr_struct;
	spin_lock(&buff_pos[nodeID].pos_lock);
	
	addr_struct.local  = (unsigned long)get_writeOut_buff(nodeID, slot);
	addr_struct.remote = (unsigned long)COMEX_writeOut_buff[nodeID][slot].remote;
	addr_struct.size   = X86PageSize;
	addr_struct.bufIDX = slot;
	
	spin_unlock(&buff_pos[nodeID].pos_lock);
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_WRTE, &addr_struct, sizeof(addr_struct));
}