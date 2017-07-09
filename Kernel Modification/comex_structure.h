int COMEX_Ready = 0;
int COMEX_ID = 0;
int COMEX_total_nodes = 0;
int COMEX_total_pages = 0;
char proc_name[100];

int COMEX_counter = 0;
char *COMEX_area;
char *COMEX_buffer_writeOut;
char *COMEX_buffer_readIn;

spinlock_t COMEX_buddy_spin;
struct semaphore COMEX_MUTEX_1;

void (*COMEX_module_echo)(char *) = NULL;
EXPORT_SYMBOL(COMEX_module_echo);