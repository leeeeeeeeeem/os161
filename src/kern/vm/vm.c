#include "types.h"
#include "lib.h"
#include "spinlock.h"
#include "vm.h"
#include "coremap.h"

bool vm_ready = false;
static struct spinlock mem_lock = SPINLOCK_INITIALIZER;

void vm_bootstrap(void) {
	coremap_init();
	vm_ready = true;
}

vaddr_t alloc_kpages(unsigned int npages) {
	spinlock_acquire(&mem_lock);
	
	vaddr_t vaddr;
	if (vm_ready == false) {
		paddr_t paddr = ram_stealmem(npages);
		vaddr =  PADDR_TO_KVADDR(paddr);
	}

	else {
		vaddr = coremap_alloc(npages);
		KASSERT(vaddr != 0);
	}

	spinlock_release(&mem_lock);

	return vaddr;
}

void free_kpages(vaddr_t addr) {
	if (vm_ready == false) {
		panic("Can't free memory before the vm system is initialized.\n");
	}

	spinlock_acquire(&mem_lock);
	coremap_free(addr);
	spinlock_release(&mem_lock);
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
	(void) faulttype;
	(void) faultaddress;
	panic("VM fault not implemented yet\n");
	return 0;
}


void vm_tlbshootdown(const struct tlbshootdown* ts) {
	(void) ts;
	panic("TLB shootdown not implemented yet\n");
}
