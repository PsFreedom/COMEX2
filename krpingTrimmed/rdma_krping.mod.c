#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xf672cfa6, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0x3f862391, __VMLINUX_SYMBOL_STR(kmalloc_caches) },
	{ 0xd2b09ce5, __VMLINUX_SYMBOL_STR(__kmalloc) },
	{ 0x349cba85, __VMLINUX_SYMBOL_STR(strchr) },
	{ 0xdfd1dc77, __VMLINUX_SYMBOL_STR(single_open) },
	{ 0x15692c87, __VMLINUX_SYMBOL_STR(param_ops_int) },
	{ 0x4b7baec6, __VMLINUX_SYMBOL_STR(ib_dealloc_pd) },
	{ 0xc8b57c27, __VMLINUX_SYMBOL_STR(autoremove_wake_function) },
	{ 0x795fc94e, __VMLINUX_SYMBOL_STR(dma_set_mask) },
	{ 0xb914bce, __VMLINUX_SYMBOL_STR(single_release) },
	{ 0x8219ee98, __VMLINUX_SYMBOL_STR(ib_dealloc_mw) },
	{ 0x60203e42, __VMLINUX_SYMBOL_STR(ib_destroy_qp) },
	{ 0x20000329, __VMLINUX_SYMBOL_STR(simple_strtoul) },
	{ 0x10e59889, __VMLINUX_SYMBOL_STR(rdma_accept) },
	{ 0x9f6b4c62, __VMLINUX_SYMBOL_STR(down_interruptible) },
	{ 0x5f2ea640, __VMLINUX_SYMBOL_STR(seq_printf) },
	{ 0xeb5cb819, __VMLINUX_SYMBOL_STR(remove_proc_entry) },
	{ 0xf33072ff, __VMLINUX_SYMBOL_STR(ib_free_fast_reg_page_list) },
	{ 0x3fec048f, __VMLINUX_SYMBOL_STR(sg_next) },
	{ 0x8dc19b65, __VMLINUX_SYMBOL_STR(rdma_destroy_id) },
	{ 0x6da3649c, __VMLINUX_SYMBOL_STR(mutex_unlock) },
	{ 0x85df9b6c, __VMLINUX_SYMBOL_STR(strsep) },
	{ 0x91715312, __VMLINUX_SYMBOL_STR(sprintf) },
	{ 0x6834dcae, __VMLINUX_SYMBOL_STR(seq_read) },
	{ 0x343a1a8, __VMLINUX_SYMBOL_STR(__list_add) },
	{ 0xe2d5255a, __VMLINUX_SYMBOL_STR(strcmp) },
	{ 0x2f995234, __VMLINUX_SYMBOL_STR(rdma_connect) },
	{ 0xf432dd3d, __VMLINUX_SYMBOL_STR(__init_waitqueue_head) },
	{ 0x2ad7b6af, __VMLINUX_SYMBOL_STR(ib_alloc_pd) },
	{ 0x33bb5312, __VMLINUX_SYMBOL_STR(current_task) },
	{ 0xd9b0ad70, __VMLINUX_SYMBOL_STR(ib_get_dma_mr) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0xfb0d48ce, __VMLINUX_SYMBOL_STR(rdma_listen) },
	{ 0x4c9d28b0, __VMLINUX_SYMBOL_STR(phys_base) },
	{ 0xa1c76e0a, __VMLINUX_SYMBOL_STR(_cond_resched) },
	{ 0xb4390f9a, __VMLINUX_SYMBOL_STR(mcount) },
	{ 0x73fb83db, __VMLINUX_SYMBOL_STR(mutex_lock) },
	{ 0x521445b, __VMLINUX_SYMBOL_STR(list_del) },
	{ 0x3aa160ae, __VMLINUX_SYMBOL_STR(rdma_create_id) },
	{ 0xc5938066, __VMLINUX_SYMBOL_STR(ib_destroy_cq) },
	{ 0xa996b740, __VMLINUX_SYMBOL_STR(rdma_create_qp) },
	{ 0x966f8d30, __VMLINUX_SYMBOL_STR(rdma_bind_addr) },
	{ 0x8eb92c54, __VMLINUX_SYMBOL_STR(module_put) },
	{ 0xebc757cd, __VMLINUX_SYMBOL_STR(rdma_resolve_route) },
	{ 0xf0fdf6cb, __VMLINUX_SYMBOL_STR(__stack_chk_fail) },
	{ 0x1000e51, __VMLINUX_SYMBOL_STR(schedule) },
	{ 0xaccabc6a, __VMLINUX_SYMBOL_STR(in4_pton) },
	{ 0x40eef28f, __VMLINUX_SYMBOL_STR(rdma_disconnect) },
	{ 0xcd758037, __VMLINUX_SYMBOL_STR(kmem_cache_alloc_trace) },
	{ 0xd25fa334, __VMLINUX_SYMBOL_STR(ib_dereg_mr) },
	{ 0xcf21d241, __VMLINUX_SYMBOL_STR(__wake_up) },
	{ 0x60ad9bfc, __VMLINUX_SYMBOL_STR(proc_create_data) },
	{ 0x89e3a00d, __VMLINUX_SYMBOL_STR(seq_lseek) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x5c8b5ce8, __VMLINUX_SYMBOL_STR(prepare_to_wait) },
	{ 0x71e3cecb, __VMLINUX_SYMBOL_STR(up) },
	{ 0xe57878a1, __VMLINUX_SYMBOL_STR(in6_pton) },
	{ 0xf1960f22, __VMLINUX_SYMBOL_STR(rdma_resolve_addr) },
	{ 0xfa66f77c, __VMLINUX_SYMBOL_STR(finish_wait) },
	{ 0xa9004097, __VMLINUX_SYMBOL_STR(ib_create_cq) },
	{ 0x77e2f33, __VMLINUX_SYMBOL_STR(_copy_from_user) },
	{ 0xb2b9919d, __VMLINUX_SYMBOL_STR(dma_ops) },
	{ 0x5ab19957, __VMLINUX_SYMBOL_STR(try_module_get) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=ib_core,rdma_cm";


MODULE_INFO(srcversion, "BC2589F3623C44EA6DBDB3C");
