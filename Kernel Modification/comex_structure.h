int COMEX_Ready = 0;
int COMEX_ID = 0;
int COMEX_total_nodes = 0;
int COMEX_total_pages = 0;
char proc_name[100];

spinlock_t COMEX_buddy_spin;
spinlock_t COMEX_freelist_spin;
struct semaphore COMEX_remote_MUTEX;

void (*COMEX_module_echo)(char *) = NULL;
EXPORT_SYMBOL(COMEX_module_echo);

void (*COMEX_verb_send)(int, int, void *, int) = NULL;
EXPORT_SYMBOL(COMEX_verb_send);

uint64_t (*COMEX_offset_to_addr)(uint64_t) = NULL;
EXPORT_SYMBOL(COMEX_offset_to_addr);