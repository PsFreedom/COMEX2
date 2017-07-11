void COMEX_init_ENV(int node_ID, int n_nodes, int writeOut_buff, int readIn_buff, int total_pages, char *namePtr)
{
	char initMSG[50];
	int i, ret=0;
	
	strcpy(proc_name, namePtr);
	printk(KERN_INFO "COMEX Kernel v.2 --> %s\n", proc_name);
	printk(KERN_INFO "ID %d n_nodes %d total_pages %d\n", node_ID, n_nodes, total_pages);
	printk(KERN_INFO "writeOut_buff %d readIn_buff %d\n", writeOut_buff, readIn_buff);
	
//	for(i=0; i<total_pages; i++){
//		printk(KERN_INFO "%d %lu --> %lu\n", i, (uint64_t)i*X86PageSize, COMEX_offset_to_addr((uint64_t)i*X86PageSize));
//	}

///// Semalphore & MUTEX
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
		COMEX_Buddy_page[i].CMX_cntr  =  0;
		COMEX_Buddy_page[i].CMX_CKSM  =  0;
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
	char *new_vAddr  = (char *)COMEX_offset_to_addr((uint64_t)COMEX_pageNO*X86PageSize);
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
	char *old_vAddr = (char *)COMEX_offset_to_addr((uint64_t)pageNO*X86PageSize);
	char *new_vAddr = (char *)kmap(new_page);
	
	memcpy(new_vAddr, old_vAddr, X86PageSize);
//	COMEX_Buddy_page[pageNO].CMX_cntr--;
//	if(COMEX_Buddy_page[pageNO].CMX_CKSM != 0)
//		printk(KERN_INFO "%s: No %d - %lu %lu\n", __FUNCTION__, pageNO, COMEX_Buddy_page[pageNO].CMX_CKSM, checkSum_page(new_page));
	
	COMEX_free_page(pageNO, 0);	
	kunmap(new_page);
}