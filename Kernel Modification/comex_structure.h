int COMEX_Ready = 0;
int COMEX_ID = 0;
int COMEX_total_nodes = 0;
int COMEX_total_pages = 0;
int COMEX_total_writeOut = 0;
int COMEX_total_readIn = 0;
char proc_name[100];

///// Counter 
unsigned long SWAP_to_Disk   = 0;
unsigned long SWAP_to_COMEX  = 0;

unsigned long COMEX_in_total = 0;
unsigned long COMEX_in_preF = 0;
unsigned long COMEX_in_buff = 0;
unsigned long COMEX_in_RDMA = 0;

struct dentry *file_SWAP_to_Disk;
struct dentry *file_SWAP_to_COMEX;

struct dentry *file_COMEX_in_total;
struct dentry *file_COMEX_in_preF;
struct dentry *file_COMEX_in_buff;
struct dentry *file_COMEX_in_RDMA;

///// Counter

spinlock_t COMEX_buddy_spin;

void (*COMEX_module_echo)(char *) = NULL;
EXPORT_SYMBOL(COMEX_module_echo);

void (*COMEX_RDMA)(int, int, void *, int) = NULL;
EXPORT_SYMBOL(COMEX_RDMA);

uint64_t (*COMEX_offset_to_addr)(uint64_t) = NULL;
EXPORT_SYMBOL(COMEX_offset_to_addr);