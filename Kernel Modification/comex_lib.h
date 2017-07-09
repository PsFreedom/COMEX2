void COMEX_init_ENV(int node_ID, int n_nodes, int writeOut_buff, int readIn_buff, int total_pages, char *namePtr, char *COMEX_addr)
{
	char initMSG[50];
	int i, ret=0;

	COMEX_ID = node_ID;
	COMEX_total_nodes = n_nodes;
	COMEX_total_pages = total_pages;
	strcpy(proc_name, namePtr);
	
	COMEX_area			  = COMEX_addr;
	COMEX_buffer_writeOut = COMEX_addr + (total_pages*X86PageSize);
	COMEX_buffer_readIn   = COMEX_addr + (total_pages*X86PageSize) + (writeOut_buff*n_nodes*X86PageSize);
	
	printk(KERN_INFO "%s: node_ID %d n_nodes %d proc_name %s\n", __FUNCTION__, COMEX_ID, COMEX_total_nodes, proc_name);
	printk(KERN_INFO "%s: writeOut_buff %d readIn_buff %d total_pages %d\n", __FUNCTION__, writeOut_buff, readIn_buff, COMEX_total_pages);
	printk(KERN_INFO "%s: COMEX_area %p\n", __FUNCTION__, COMEX_area);
	printk(KERN_INFO "%s: COMEX_buffer_writeOut %p\n", __FUNCTION__, COMEX_buffer_writeOut);
	printk(KERN_INFO "%s: COMEX_buffer_readIn %p\n", __FUNCTION__, COMEX_buffer_readIn);
	
	sema_init(&COMEX_MUTEX_1, 1);
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
	sprintf(initMSG,"Finish initialization...");
	COMEX_module_echo(initMSG);
	COMEX_Ready = 1;
}
EXPORT_SYMBOL(COMEX_init_ENV);

int COMEX_move_to_COMEX(struct page *old_page, int *retNodeID, unsigned long *retPageNO)
{
	int COMEX_pageNO = COMEX_rmqueue_smallest(0);
	char *new_vAddr  = COMEX_area + ((long)COMEX_pageNO*X86PageSize);
	char *old_vAddr  = (char *)kmap(old_page);
	
	if(COMEX_pageNO < 0){	// No page available
		return -1;
	}
	
	memcpy(new_vAddr, old_vAddr, X86PageSize);
//	COMEX_Buddy_page[COMEX_pageNO].CMX_cntr++;
//	COMEX_Buddy_page[COMEX_pageNO].CMX_CKSM = checkSum_page(old_page);
//	if(checkSum_page(old_page) != 0)
//		printk(KERN_INFO "%s: No %d - %lu %lu %lu\n", __FUNCTION__, COMEX_pageNO, checkSum_page(old_page), checkSum_page(vmalloc_to_page(new_vAddr)), checkSum_Vpage(new_vAddr));
	
	kunmap(old_page);
	*retNodeID = -11;
	*retPageNO = (unsigned long)COMEX_pageNO;
	return 1;
}

void COMEX_read_from_local(struct page *new_page, int pageNO)
{
	char *old_vAddr = COMEX_area + ((long)pageNO*X86PageSize);
	char *new_vAddr = (char *)kmap(new_page);
	
	memcpy(new_vAddr, old_vAddr, X86PageSize);
//	COMEX_Buddy_page[pageNO].CMX_cntr--;
//	if(COMEX_Buddy_page[pageNO].CMX_CKSM != 0)
//		printk(KERN_INFO "%s: No %d - %lu %lu\n", __FUNCTION__, pageNO, COMEX_Buddy_page[pageNO].CMX_CKSM, checkSum_page(new_page));
	
	COMEX_free_page(pageNO, 0);	
	kunmap(new_page);
}