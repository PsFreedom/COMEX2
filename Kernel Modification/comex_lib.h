void COMEX_init_Remote()
{
	int i,j;
	printk(KERN_INFO "%s... Begin\n", __FUNCTION__);
	
	COMEX_free_group = (COMEX_R_free_group_t *)vmalloc(sizeof(COMEX_R_free_group_t)*COMEX_total_nodes);
	for(i=0; i<COMEX_total_nodes; i++){
		COMEX_free_group[i].mssg_qouta  = MAX_MSSG;
		COMEX_free_group[i].total_group = 0;
		COMEX_free_group[i].back_off 	= 0;
		
		spin_lock_init(&COMEX_free_group[i].list_lock);
		INIT_LIST_HEAD(&COMEX_free_group[i].free_group);
		
		if(list_empty(&COMEX_free_group[i].free_group))
			printk(KERN_INFO "%d... list_empty!\n", i);
	}
	
	buff_pos = (buff_pos_t *)vmalloc(sizeof(buff_pos_t)*COMEX_total_nodes);
	memset(buff_pos, 0, sizeof(buff_pos_t)*COMEX_total_nodes);

	COMEX_writeOut_buff = (buff_desc_t **)vmalloc(sizeof(buff_desc_t *)*COMEX_total_nodes);
	for(i=0; i<COMEX_total_nodes; i++){
		COMEX_writeOut_buff[i] = (buff_desc_t *)vmalloc(sizeof(buff_desc_t)*COMEX_total_writeOut);
		for(j=0; j<COMEX_total_writeOut; j++){
			COMEX_writeOut_buff[i][j].status = -1;
			COMEX_writeOut_buff[i][j].nodeID = -1;
			COMEX_writeOut_buff[i][j].pageNO = -1;
			
			printk(KERN_INFO "writeOut %d %d: %hhd %d %d\n", i, j, COMEX_writeOut_buff[i][j].status, COMEX_writeOut_buff[i][j].nodeID, COMEX_writeOut_buff[i][j].pageNO);
		}
	}
	
	COMEX_readIn_buff = (buff_desc_t *)vmalloc(sizeof(buff_desc_t)*COMEX_total_readIn);
	for(i=0; i<COMEX_total_readIn; i++){
		COMEX_readIn_buff[i].status = -1;
		COMEX_readIn_buff[i].nodeID = -1;
		COMEX_readIn_buff[i].pageNO = -1;
		
		printk(KERN_INFO "readIn %d: %hhd %d %d\n", i, COMEX_readIn_buff[i].status, COMEX_readIn_buff[i].nodeID, COMEX_readIn_buff[i].pageNO);
	}
	
	COMEX_free_struct = (free_struct_t *)vmalloc(sizeof(free_struct_t)*COMEX_total_nodes);
	for(i=0; i<COMEX_total_nodes; i++){
		for(j=0; j<MAX_FREE; j++){
			COMEX_free_struct[i].pageNO[j] = -1;
			COMEX_free_struct[i].count[j]  =  0;	
		}
	}
}

void COMEX_init_ENV(int node_ID, int n_nodes, int writeOut_buff, int readIn_buff, int total_pages, char *namePtr)
{
	char initMSG[50];
	int i, ret=0;
	
	COMEX_ID			 = node_ID;
	COMEX_total_nodes	 = n_nodes;
	COMEX_total_pages	 = total_pages;
	COMEX_total_writeOut = writeOut_buff;
	COMEX_total_readIn   = readIn_buff;
	
//	COMEX_total_nodes	 = 4;
//	COMEX_total_writeOut = 64;
//	COMEX_total_readIn   = 256;
	
	strcpy(proc_name, namePtr);
	printk(KERN_INFO "COMEX Kernel v.2 --> %s\n", proc_name);
	printk(KERN_INFO "ID %d n_nodes %d total_pages %d\n", node_ID, n_nodes, total_pages);
	printk(KERN_INFO "writeOut_buff %d readIn_buff %d\n", writeOut_buff, readIn_buff);
	
//	for(i=0; i<total_pages; i++){
//		printk(KERN_INFO "%d %lu --> %lu\n", i, (uint64_t)i*X86PageSize, COMEX_offset_to_addr((uint64_t)i*X86PageSize));
//	}

///// Semalphore & MUTEX
	spin_lock_init(&COMEX_buddy_spin);
	
///// Buddy System
	for(i=0; i<COMEX_MAX_ORDER; i++){
		COMEX_free_area[i].nr_free = 0;
		INIT_LIST_COMEX(&COMEX_free_area[i].free_list);
	}
	
	COMEX_Buddy_page = (COMEX_page *)vmalloc(sizeof(COMEX_page)*total_pages);
	for(i=0; i<total_pages; i++){
		COMEX_Buddy_page[i].pageNO    =  i;
		COMEX_Buddy_page[i].private   =  0;
		COMEX_Buddy_page[i]._mapcount = -1;
//		COMEX_Buddy_page[i].CMX_cntr  =  0;
//		COMEX_Buddy_page[i].CMX_CKSM  =  0;
		INIT_LIST_COMEX(&COMEX_Buddy_page[i].lru);
	}
	
	for(i=0; i<total_pages; i++){
		COMEX_free_page(i,0);
	}
	while(ret >= 0){
		ret = COMEX_rmqueue_smallest(0);
	}
	for(i=0; i<total_pages; i++){
		COMEX_free_page(i,0);
	}

///// Footer
	COMEX_init_Remote();
	sprintf(initMSG,"Finish initialization...");
	COMEX_module_echo(initMSG);
	COMEX_Ready = 1;
}
EXPORT_SYMBOL(COMEX_init_ENV);

int COMEX_move_to_COMEX(struct page *old_page, int *retNodeID, unsigned long *retPageNO)
{
	long COMEX_pageNO = (long)COMEX_rmqueue_smallest(0);
	char *new_vAddr;
	char *old_vAddr;
	
	if(COMEX_pageNO < 0 || COMEX_pageNO >= COMEX_total_pages){	// No page available
//		printk(KERN_INFO "%s: Wrong pageNO %ld\n", __FUNCTION__, COMEX_pageNO);
		return -1;
	}
	new_vAddr  = (char *)COMEX_offset_to_addr(COMEX_pageNO*X86PageSize);
	old_vAddr  = (char *)kmap(old_page);
	
	memcpy(new_vAddr, old_vAddr, X86PageSize);
//	COMEX_Buddy_page[COMEX_pageNO].CMX_cntr++;
//	COMEX_Buddy_page[COMEX_pageNO].CMX_CKSM = checkSum_page(old_page);
//	if(checkSum_page(old_page) != 0)
//		printk(KERN_INFO "%s: No %d - %lu %lu %lu\n", __FUNCTION__, COMEX_pageNO, checkSum_page(old_page), checkSum_page(vmalloc_to_page(new_vAddr)), checkSum_Vpage(new_vAddr));
	
	kunmap(old_page);
	*retNodeID = -11;
	*retPageNO = COMEX_pageNO;
	return 1;
}

void COMEX_read_from_local(struct page *new_page, unsigned long pageNO)
{
	char *old_vAddr;
	char *new_vAddr;
	
	if(pageNO < 0 || pageNO >= COMEX_total_pages){	// No page available
		printk(KERN_INFO "%s: Wrong pageNO %ld\n", __FUNCTION__, pageNO);
		return;
	}
	
	old_vAddr = (char *)COMEX_offset_to_addr(pageNO*X86PageSize);
	new_vAddr = (char *)kmap(new_page);
	
	memcpy(new_vAddr, old_vAddr, X86PageSize);
//	COMEX_Buddy_page[pageNO].CMX_cntr--;
//	if(COMEX_Buddy_page[pageNO].CMX_CKSM != 0)
//		printk(KERN_INFO "%s: No %d - %lu %lu\n", __FUNCTION__, pageNO, COMEX_Buddy_page[pageNO].CMX_CKSM, checkSum_page(new_page));
	
	COMEX_free_page(pageNO, 0);	
	kunmap(new_page);
}