#define MAX_TRY 5
#define MAX_MSSG 8
#define FLUSH 32
#define PreF_BITS 4
#define PreF_SIZE (1UL << PreF_BITS)	// 0000 1000 (When PreF_BITS == 3)
#define PreF_MASK (~(PreF_SIZE - 1))	// 0000 0111 -> 1111 1000

#define Hash_BITS 24
#define Hash_MASK ((1UL << Hash_BITS) - 1)

#define	HASH 255
#define HASH_Shift 8

typedef struct{
	int mssg_qouta;
	int total_group;
	int total_page;
	int back_off;
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
	struct mutex pos_mutex;
} buff_pos_t;
buff_pos_t *buff_pos;

free_struct_t *COMEX_free_struct;

int COMEX_freelist_getpage(int list_ID);
void COMEX_free_to_remote(int nodeID, int pageNO);
void COMEX_flush_buff(int nodeID);
void COMEX_flush_one(int nodeID, int slot);
void COMEX_void_preFetch(int nodeID, int pageNO);

////////////////////

void COMEX_freelist_print(int nodeID){
	free_group_t *myPtr;
	printk(KERN_INFO "List NO - %d\n", nodeID);
	list_for_each_entry(myPtr, &COMEX_free_group[nodeID].free_group, link){
		printk(KERN_INFO " >>> %d - %d\n", myPtr->page_start, myPtr->page_end);
	}
}

int COMEX_hash(int seed){
//	return 1;
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
	
	for(i=0; i<COMEX_total_nodes; i++)
	{
		if(mutex_trylock(&COMEX_free_group[dest_node].mutex_FG) == 0)
			return -1;
		
		if( COMEX_free_group[dest_node].total_page <= 512 && 
            COMEX_free_group[dest_node].mssg_qouta  > 0)
		{
			if( COMEX_free_group[dest_node].back_off <= 0){
				COMEX_free_group[dest_node].mssg_qouta--;
				COMEX_free_group[dest_node].back_off += 1UL<<(12 - COMEX_free_group[dest_node].mssg_qouta);
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
			mutex_unlock(&COMEX_free_group[dest_node].mutex_FG);
			COMEX_void_preFetch(dest_node, *retPageNO);
			
			buf_vAddr = (char *)get_writeOut_buff(dest_node, buff_slot);
			old_vAddr = (char *)kmap(old_page);
			memcpy((char *)COMEX_offset_to_addr((uint64_t)buf_vAddr), old_vAddr, X86PageSize);
			kunmap(old_page);
			
			COMEX_writeOut_buff[dest_node][buff_slot].nodeID = dest_node;
			COMEX_writeOut_buff[dest_node][buff_slot].pageNO = *retPageNO;
			COMEX_writeOut_buff[dest_node][buff_slot].status = 2;
		//	COMEX_flush_one(dest_node, buff_slot);
		//	mutex_unlock(&COMEX_free_group[dest_node].mutex_FG);
			if(buff_slot%FLUSH == 0 && buff_slot != 0)
                COMEX_flush_buff(dest_node);

			*retNodeID = dest_node;
			return 1;
		}
		mutex_unlock(&COMEX_free_group[dest_node].mutex_FG);
		dest_node = COMEX_hash(dest_node);
	}
	*retNodeID = 0;
	*retPageNO = 0;
	return -1;
}

int COMEX_read_from_buffer(struct page *new_page, int nodeID, int pageNO)
{
	int i;
	char *buf_vAddr;
	char *new_vAddr;
	
	for(i=0; i<COMEX_total_writeOut; i++){
		if(COMEX_writeOut_buff[nodeID][i].pageNO == pageNO &&
		   COMEX_writeOut_buff[nodeID][i].status == 2)
		{
			buf_vAddr = (char *)get_writeOut_buff(nodeID, i);
			new_vAddr = (char *)kmap(new_page);
			memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)buf_vAddr), X86PageSize);
			kunmap(new_page);
			
	//		printk(KERN_INFO "%s: Hit Buffer!... %d %d\n", __FUNCTION__, nodeID, pageNO);
			COMEX_in_buff++;
			return 1;
		}
	}
	return 0;
}

int COMEX_read_from_preFetch(struct page *new_page, int nodeID, int pageNO)
{
	int index = pageNO%COMEX_total_readIn;
	char *buf_vAddr;
	char *new_vAddr;
	
	if(COMEX_readIn_buff[index].status == 2      &&
	   COMEX_readIn_buff[index].nodeID == nodeID &&
	   COMEX_readIn_buff[index].pageNO == pageNO )
	{
		buf_vAddr = (char *)get_readIn_buff(index);
		new_vAddr = (char *)kmap(new_page);
		memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)buf_vAddr), X86PageSize);
		kunmap(new_page);
		
	//	printk(KERN_INFO "%s: Hit preFetch!... %d %d\n", __FUNCTION__, nodeID, pageNO);
		COMEX_in_preF++;
		return 1;
	}
	return 0;
}
/*
int COMEX_read_from_preFetch_hash(struct page *new_page, int nodeID, int pageNO)
{
	int i, index;
	unsigned long hashKey  = 0UL + nodeID<<24 + pageNO&Hash_MASK;
	char *buf_vAddr;
	char *new_vAddr;

	for(i=0; i<4; i++){
		index = hashKey & HASH;
		if(COMEX_readIn_buff[index].nodeID == nodeID &&
		   COMEX_readIn_buff[index].pageNO == pageNO )
		{
			buf_vAddr = (char *)get_readIn_buff(index);
			new_vAddr = (char *)kmap(new_page);
			memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)buf_vAddr), X86PageSize);
			kunmap(new_page);
			
			printk(KERN_INFO "%s: Hit preFetch!... %d %d\n", __FUNCTION__, nodeID, pageNO);
			COMEX_in_preF++;
			return 1;
		}
		hashKey  = hashKey >> HASH_Shift;
	}
	return 0;
}

void COMEX_read_from_remote_hash(struct page *new_page, int nodeID, int pageNO)
{
	COMEX_address_t addr_struct;
	int i, buff_IDX ,buff_FLR;
	int page_FLR = pageNO & PreF_MASK;
	char *buf_vAddr, *new_vAddr;
	unsigned long hashKey;
	
COMEX_retry:
	hashKey  = 0UL + nodeID<<24 + pageNO&Hash_MASK;
	for(i=0; i<4; i++){
		buff_IDX = hashKey & HASH;
		buff_FLR = buff_IDX & PreF_MASK;
		if(COMEX_readIn_buff[buff_FLR].status == 2){
			COMEX_readIn_buff[buff_FLR].status = -1;
			break;
		}
		hashKey  = hashKey >> HASH_Shift;
	}
	if(i == 4){
		printk(KERN_INFO "%s: HASH Colision!\n", __FUNCTION__);
		goto COMEX_retry;
	}
	
	for(i=0; i<PreF_SIZE; i++){
		COMEX_readIn_buff[buff_FLR + i].status = -1;
		COMEX_readIn_buff[buff_FLR + i].nodeID = -1;
		COMEX_readIn_buff[buff_FLR + i].pageNO = -1;
	}
	
	buf_vAddr = (char *)get_readIn_buff(buff_FLR);
	new_vAddr = (char *)kmap(new_page);
	addr_struct.local  = (unsigned long)buf_vAddr;
	addr_struct.remote = (unsigned long)page_FLR << SHIFT_PAGE;
	addr_struct.size   = PreF_SIZE;
	addr_struct.bufIDX = buff_FLR;
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_READ, &addr_struct, sizeof(addr_struct));
	memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)get_readIn_buff(buff_IDX)), X86PageSize);

	for(i=0; i<PreF_SIZE; i++){
		COMEX_readIn_buff[buff_FLR + i].status = 2;
		COMEX_readIn_buff[buff_FLR + i].nodeID = nodeID;
		COMEX_readIn_buff[buff_FLR + i].pageNO = page_FLR + i;
	}
	COMEX_in_RDMA++;
}
*/
void COMEX_read_from_remote(struct page *new_page, int nodeID, int pageNO)
{
	int i;
	int buff_IDX = pageNO % COMEX_total_readIn;
	int buff_FLR = buff_IDX & PreF_MASK;
	int page_FLR = pageNO & PreF_MASK;
	
	char *buf_vAddr = (char *)get_readIn_buff(buff_FLR);
	char *new_vAddr = (char *)kmap(new_page);
	COMEX_address_t addr_struct;

	mutex_lock(&COMEX_readIn_buff[buff_FLR].mutex_buff);
	for(i=0; i<PreF_SIZE; i++){
		COMEX_readIn_buff[buff_FLR + i].status = -1;
		COMEX_readIn_buff[buff_FLR + i].nodeID = -1;
		COMEX_readIn_buff[buff_FLR + i].pageNO = -1;
	}
	
	addr_struct.local  = (unsigned long)buf_vAddr;
	addr_struct.remote = (unsigned long)page_FLR << SHIFT_PAGE;
	addr_struct.size   = PreF_SIZE;
	addr_struct.bufIDX = buff_FLR;
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_READ, &addr_struct, sizeof(addr_struct));
	memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)get_readIn_buff(buff_IDX)), X86PageSize);

	for(i=0; i<PreF_SIZE; i++){
		COMEX_readIn_buff[buff_FLR + i].status = 2;
		COMEX_readIn_buff[buff_FLR + i].nodeID = nodeID;
		COMEX_readIn_buff[buff_FLR + i].pageNO = page_FLR + i;
	}
	mutex_unlock(&COMEX_readIn_buff[buff_FLR].mutex_buff);
	kunmap(new_page);
	COMEX_in_RDMA++;
}

void COMEX_read_from_remote_one(struct page *new_page, int nodeID, int pageNO)
{
	int i;
	int buff_IDX = pageNO % COMEX_total_readIn;
	char *buf_vAddr = (char *)get_readIn_buff(buff_IDX);
	char *new_vAddr = (char *)kmap(new_page);
	COMEX_address_t addr_struct;

	mutex_lock(&mutex_PF);
	
	addr_struct.local  = (unsigned long)buf_vAddr;
	addr_struct.remote = (unsigned long)pageNO << SHIFT_PAGE;
	addr_struct.size   = 1;
	addr_struct.bufIDX = buff_IDX;
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_READ, &addr_struct, sizeof(addr_struct));
	memcpy(new_vAddr, (char *)COMEX_offset_to_addr((uint64_t)buf_vAddr), X86PageSize);
	
	mutex_unlock(&mutex_PF);
	
	kunmap(new_page);
	COMEX_in_RDMA++;
}

void COMEX_page_receive(int nodeID, int pageNO, int group_size)
{
	mutex_lock(&COMEX_free_group[nodeID].mutex_FG);
	if(pageNO >= 0){
		free_group_t *group_ptr = (free_group_t *)kzalloc(sizeof(free_group_t), GFP_KERNEL);
		
		group_ptr->page_start = pageNO;
		group_ptr->page_end   = pageNO + (1UL<<group_size) - 1;
		INIT_LIST_HEAD(&group_ptr->link);

	//	printk(KERN_INFO "%s: %d %d - %d\n", __FUNCTION__, nodeID, group_ptr->page_start, group_ptr->page_end);
		list_add_tail(&group_ptr->link, &COMEX_free_group[nodeID].free_group);
		COMEX_free_group[nodeID].mssg_qouta = MAX_MSSG;
		COMEX_free_group[nodeID].total_group++;
		COMEX_free_group[nodeID].total_page += (1UL<<group_size);
	}
	else{
		COMEX_free_group[nodeID].mssg_qouta = MAX_MSSG;
		COMEX_free_group[nodeID].back_off += 50000;
	}
	mutex_unlock(&COMEX_free_group[nodeID].mutex_FG);
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
	}
    
    COMEX_free_group[list_ID].total_page--;
//	printk(KERN_INFO "%s: %d | %d %d - %d\n", __FUNCTION__, list_ID, ret_pageNO, myPtr->page_start, myPtr->page_end);
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
		if( COMEX_free_struct[nodeID].pageNO[i] + COMEX_free_struct[nodeID].count[i] == pageNO && 
			COMEX_free_struct[nodeID].count[i] < 16383)
		{
			COMEX_free_struct[nodeID].count[i]++;
			spin_unlock(&freePage_spin);
			return;
		}
		if( COMEX_free_struct[nodeID].pageNO[i] - 1 == pageNO && 
			COMEX_free_struct[nodeID].count[i] < 16383)
		{
			COMEX_free_struct[nodeID].pageNO[i]--;
			COMEX_free_struct[nodeID].count[i]++;
			spin_unlock(&freePage_spin);
			return;
		}
		if( COMEX_free_struct[nodeID].pageNO[i] == -1)
		{
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
	while(myStruct.page_no < 0 && i >= 5){
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
		COMEX_writeOut_buff[nodeID][pageNO].nodeID = -1;
		COMEX_writeOut_buff[nodeID][pageNO].pageNO = -1;
		COMEX_writeOut_buff[nodeID][pageNO].status = -1;
		con_page--;
		pageNO++;
	}
}
EXPORT_SYMBOL(COMEX_free_buff);

void COMEX_void_preFetch(int nodeID, int pageNO)
{
	int index = pageNO%COMEX_total_readIn;
	
	if(COMEX_readIn_buff[index].nodeID == nodeID &&
	   COMEX_readIn_buff[index].pageNO == pageNO )
	{
		COMEX_readIn_buff[index].status = -1;
		COMEX_readIn_buff[index].nodeID = -1;
		COMEX_readIn_buff[index].pageNO = -1;
	}
}

void COMEX_flush_buff(int nodeID)
{
	int count = 0;
	COMEX_address_t addr_struct;
	
	mutex_lock(&buff_pos[nodeID].pos_mutex);
	while(COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].status == 2)
	{
		COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].status = 3;
		count++;

		if(buff_pos[nodeID].head + count >= COMEX_total_writeOut){
			break;
		}
		if(	(COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].pageNO % 1024) - 1 !=
			(COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count -1].pageNO % 1024)  && 
			COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].status == 2 )
		{
			printk(KERN_INFO "%s: Not contiguous %d %d\n", __FUNCTION__, 
						COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count -1].pageNO,
						COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head + count].pageNO);
			break;
		}
	}
	if(count == 0){
		mutex_unlock(&buff_pos[nodeID].pos_mutex);
		return;
	}
	
	addr_struct.local  = (unsigned long)get_writeOut_buff(nodeID, buff_pos[nodeID].head);
	addr_struct.remote = (unsigned long)COMEX_writeOut_buff[nodeID][buff_pos[nodeID].head].pageNO << SHIFT_PAGE;
	addr_struct.size   = count;
	addr_struct.bufIDX = buff_pos[nodeID].head;
	
	buff_pos[nodeID].head += count;
	buff_pos[nodeID].head %= COMEX_total_writeOut;
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_WRTE, &addr_struct, sizeof(addr_struct));
	mutex_unlock(&buff_pos[nodeID].pos_mutex);
}

void COMEX_flush_one(int nodeID, int slot)
{
	COMEX_address_t addr_struct;
	addr_struct.local  = (unsigned long)get_writeOut_buff(nodeID, slot);
	addr_struct.remote = (unsigned long)COMEX_writeOut_buff[nodeID][slot].pageNO << SHIFT_PAGE;
	addr_struct.size   = 1;
	addr_struct.bufIDX = slot;
	COMEX_RDMA(nodeID, CODE_COMEX_PAGE_WRTE, &addr_struct, sizeof(addr_struct));
}
