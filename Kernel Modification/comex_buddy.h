#define Biggest_Group 1024

#define COMEX_list_entry(ptr, type, member) ({                      \
			const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
			(type *)( (char *)__mptr - offsetof(type,member) );})

struct COMEX_list_head {
	struct COMEX_list_head *next, *prev;
};

typedef struct COMEX_free_area_type {
	int nr_free;
	struct COMEX_list_head free_list;
} COMEX_free_area_t;
COMEX_free_area_t COMEX_free_area[COMEX_MAX_ORDER];

typedef struct Dummy_page {
	int pageNO;	
	int private;
	int _mapcount;
	struct COMEX_list_head lru;
	
	int CMX_cntr;			// TEMP
//	unsigned long CMX_CKSM;	// TEMP
} COMEX_page;
COMEX_page *COMEX_Buddy_page;

//////////////////// COMEX Buddy system Structure ////////////////////

static inline void INIT_LIST_COMEX(struct COMEX_list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline int COMEX__list_empty(const struct COMEX_list_head *head)
{
	return head->next == head;
}

static inline void COMEX__list_add(struct COMEX_list_head *new, struct COMEX_list_head *prev, struct COMEX_list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void COMEX_list_add(struct COMEX_list_head *new, struct COMEX_list_head *head)
{
	COMEX__list_add(new, head, head->next);
}

static inline void COMEX_list_add_tail(struct COMEX_list_head *new, struct COMEX_list_head *head)
{
	COMEX__list_add(new, head->prev, head);
}

static inline void COMEX__list_del(struct COMEX_list_head * prev, struct COMEX_list_head * next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void COMEX_list_del(struct COMEX_list_head *entry)
{
	COMEX__list_del(entry->prev, entry->next);
	entry->next = entry;
	entry->prev = entry;
}

static inline void COMEX_set_page_order(COMEX_page *page, int order)
{
	page->private = order;
	page->_mapcount = -128;
}

static inline void COMEX_rmv_page_order(COMEX_page *page)
{
	page->private = 0;
	page->_mapcount = -1;
}

static inline int COMEX_page_is_buddy(COMEX_page *buddy, int order)
{
	if ( buddy->_mapcount == (-128) && buddy->private == order ){
		return 1;
	}
	return 0;
}

static inline void COMEX_expand(COMEX_page *page, int low, int high, COMEX_free_area_t *area)
{
	unsigned long size = 1 << high;
	while (high > low) {
		area--;
		high--;
		size >>= 1;
		COMEX_list_add(&page[size].lru, &area->free_list);
		area->nr_free++;
		COMEX_set_page_order(&page[size], high);
	}
}

int COMEX_checkCtr(int pageNO, int order, int val)
{
	int i;
	for(i=0; i<(1UL<<order); i++)
	{
		COMEX_Buddy_page[pageNO+i].CMX_cntr += val;
		
		if(val == 1 && COMEX_Buddy_page[pageNO+i].CMX_cntr != 1){
			printk(KERN_INFO "Alloc %d FAILED! - val %d counter %d\n", pageNO+i, val, COMEX_Buddy_page[pageNO+i].CMX_cntr);
			return 0;
		}
		else if(val == -1 && COMEX_Buddy_page[pageNO+i].CMX_cntr != 0){
			printk(KERN_INFO "Free %d FAILED! - val %d counter %d\n", pageNO+i, val, COMEX_Buddy_page[pageNO+i].CMX_cntr);
			return 0;
		}
	}
	return 1;
}

static inline int COMEX_rmqueue_smallest(int order)
{
	int current_order;
	COMEX_free_area_t *area;
	COMEX_page *page;
	
	spin_lock(&COMEX_buddy_spin);
	/* Find a page of the appropriate size in the preferred list */
	for (current_order = order; current_order < COMEX_MAX_ORDER; ++current_order)
	{
		area = &COMEX_free_area[current_order];
		if(COMEX__list_empty(&area->free_list)){
			continue;
		}
		
		page = COMEX_list_entry(area->free_list.next, COMEX_page, lru);
		COMEX_list_del(&page->lru);
		COMEX_rmv_page_order(page);
		area->nr_free--;
		COMEX_expand(page, order, current_order, area);
		
//		printk(KERN_INFO "%s: %u - %d %u\n", __FUNCTION__, current_order, area->nr_free, page->pageNO);
		if(COMEX_checkCtr(page->pageNO, order, 1) == 0){
			spin_unlock(&COMEX_buddy_spin);
			return -1;
		}

		spin_unlock(&COMEX_buddy_spin);
		return page->pageNO;
	}
	spin_unlock(&COMEX_buddy_spin);
	return -1;
}

void COMEX_free_page(int inPageNO, int order)
{
	int page_idx = 0;
	int buddy_idx = 0;
	int combined_idx = 0;
	
	COMEX_page *page = &COMEX_Buddy_page[inPageNO];
	COMEX_page *buddy;

	spin_lock(&COMEX_buddy_spin);
	if(COMEX_checkCtr(inPageNO, order, -1) == 0){
		spin_unlock(&COMEX_buddy_spin);
		return;
	}
	
	page_idx = page->pageNO & ((1 << COMEX_MAX_ORDER) - 1);
	while (order < COMEX_MAX_ORDER-1){
		buddy_idx = page_idx ^ (1 << order);
		buddy     = page + (buddy_idx - page_idx);
		
		if(COMEX_page_is_buddy( buddy, order) == 0)
			break;

		COMEX_list_del(&buddy->lru);
		COMEX_rmv_page_order(buddy);
		COMEX_rmv_page_order(page);
		COMEX_free_area[order].nr_free--;
		
		combined_idx = page_idx & ~(1 << order);
		page = page + (combined_idx - page_idx);
		page_idx = combined_idx;
		order++;
	}
	COMEX_set_page_order(page, order);
	COMEX_list_add_tail(&page->lru, &COMEX_free_area[order].free_list);
	COMEX_free_area[order].nr_free++;
	
	spin_unlock(&COMEX_buddy_spin);
}
EXPORT_SYMBOL(COMEX_free_page);

void print_nr_free(void)
{
	int i;
	for(i=0; i<COMEX_MAX_ORDER; i++){
		printk(KERN_INFO "%s: Order %d - %d\n", __FUNCTION__, i, COMEX_free_area[i].nr_free);
	}
}