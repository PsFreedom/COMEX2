unsigned long checkSum_page(struct page *page)
{
	int i;
	unsigned char *chrPtr = (unsigned char *)kmap(page);
	unsigned long ret = 0;
	for(i=0 ; i<X86PageSize; i++){
		ret += chrPtr[i];
	}
	kunmap(page);
	return ret;
}
EXPORT_SYMBOL(checkSum_page);

unsigned long checkSum_Vpage(unsigned char *chrPtr)
{
	int i;
	unsigned long ret = 0;
	for(i=0 ; i<X86PageSize; i++){
		ret += chrPtr[i];
	}
	return ret;
}
EXPORT_SYMBOL(checkSum_Vpage);

struct task_struct * get_taskStruct(struct page *page)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	struct task_struct *myTask = NULL;
	pgoff_t pgoff;
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	
	anon_vma = page_lock_anon_vma_read(page);
	if (!anon_vma){
		return NULL;
	}

	pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff)
	{
		if(!avc)
			break;
		vma = avc->vma;
		if(!vma)
			break;
		mm = vma->vm_mm;
		if(!mm)
			break;
		myTask = mm->owner;	
		break;
	}	
	page_unlock_anon_vma_read(anon_vma);
	return myTask;
}

int get_page_PID(struct page *page)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	struct task_struct *myTask = NULL;
	pgoff_t pgoff;
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	
	anon_vma = page_lock_anon_vma_read(page);
	if (!anon_vma){
		return -1;
	}

	pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff)
	{
		if(!avc)
			break;
		vma = avc->vma;
		if(!vma)
			break;
		mm = vma->vm_mm;
		if(!mm)
			break;
		myTask = mm->owner;	
		break;
	}	
	page_unlock_anon_vma_read(anon_vma);
	return (int)myTask->pid;
}