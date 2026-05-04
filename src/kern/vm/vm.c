#include <addrspace.h>
#include <kern/errno.h>
#include <pagetable.h>
#include <proc.h>
#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <vm.h>
#include <coremap.h>
#include <mips/tlb.h>
#include <spl.h>

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
		if (vaddr == 0)
			return 0;
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
	struct addrspace* as = proc_getas();
	paddr_t paddr;

	if (as == NULL)
		return EFAULT;

	if (faultaddress == 0 || faulttype > 2)
		return EFAULT;

	struct region* region = as->regions; 
	struct region* found_region = NULL;
	while (region != NULL) {
		if (faultaddress >= region->vaddr && 
			faultaddress < region->vaddr + (region->npages * PAGE_SIZE)){
				found_region = region;
				break;
		}
		region = region->next;
	}

	if (found_region == NULL){
		if (faultaddress >= as->stack_base - (as->stack_npages * PAGE_SIZE) &&
			faultaddress < as->stack_base)
				goto translate;
		return EFAULT;
	}

	switch (faulttype) {
		case VM_FAULT_READONLY:
			if (found_region->writeable == 0)
				return EFAULT;
			break;
		case VM_FAULT_READ:
			if (found_region->readable == 0 && found_region->executable == 0)
				return EFAULT;
			break;
		case VM_FAULT_WRITE:
			if (found_region->writeable == 0)
				return EFAULT;
			break;
	}

translate:
	paddr = pagetable_translate(as->pagetable, faultaddress);
	if (paddr == 0)
		return ENOMEM;

	paddr |= TLBLO_VALID;

	if (found_region == NULL || found_region->writeable == 1)
		paddr |= TLBLO_DIRTY;

	int spl = splhigh();
	tlb_random(faultaddress & PAGE_FRAME, paddr);
	splx(spl);

	return 0;
}


void vm_tlbshootdown(const struct tlbshootdown* ts) {
	(void) ts;
	panic("TLB shootdown not implemented yet\n");
}
