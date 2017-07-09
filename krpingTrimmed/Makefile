KSRC=/lib/modules/`uname -r`/build
KOBJ=/lib/modules/`uname -r`/build

obj-m += rdma_krping.o
rdma_krping-y			:= getopt.o krping.o

default:
	cp /usr/src/ofa_kernel/default/Module.symvers .
	make -C $(KSRC) M=`pwd` modules
	
install:
	make -C $(KSRC) M=`pwd` modules_install
	depmod -a
copy_symvers:
	cp /usr/src/ofa_kernel/default/Module.symvers .
#	cp /usr/src/ofa_kernel/Module.symvers .
clean:
	rm -f *.o
	rm -f *.ko
	rm -f rdma_krping.mod.c
	rm -f Module.symvers
	rm -f Module.markers

