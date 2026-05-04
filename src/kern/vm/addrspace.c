/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "pagetable.h"
#include "spl.h"
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include "../../arch/mips/include/tlb.h"

#define USER_STACK_SIZE 16

struct addrspace *as_create(void) {
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->regions = NULL;

	as->pagetable = pagetable_create();
	if (as->pagetable == NULL)
		return NULL;
	
	return as;
}

int as_copy(struct addrspace *old, struct addrspace **ret) {
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}
	
	newas->stack_base = old->stack_base;
	newas->stack_npages = old->stack_npages;
	newas->pagetable = pagetable_copy(old->pagetable);
	if (newas->pagetable == NULL && old->pagetable != NULL) {
		as_destroy(newas);
		return ENOMEM;
	}
	newas->regions = NULL;

	struct region* region = old->regions;
	while(region != NULL) {
		struct region* new_region = kmalloc(sizeof(struct region));
		if (new_region == NULL){
			as_destroy(newas);
			return ENOMEM;
		}

		new_region->readable = region->readable;
		new_region->writeable = region->writeable;
		new_region->writeable_backup = region->writeable_backup;
		new_region->executable = region->executable;
		new_region->vaddr = region->vaddr;
		new_region->npages = region->npages;

		new_region->next = newas->regions;
		newas->regions = new_region;

		region = region->next;
	}

	*ret = newas;
	return 0;
}

void as_destroy(struct addrspace *as) {

	struct region* region = as->regions;
	while(region != NULL) {
		struct region* next = region->next;
		kfree(region);
		region = next;
	}

	pagetable_destroy(as->pagetable);

	kfree(as);
}

void as_activate(void) {
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		/*
		 * Kernel thread without an address space; leave the
		 * prior address space in place.
		 */
		return;
	}

	int spl = splhigh();

	for (int i = 0; i < NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void as_deactivate(void) {
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
					 int readable, int writeable, int executable) {

	struct region* regions_list = as->regions;
	struct region* region;
	
	region = kmalloc(sizeof(struct region));
	if (region == NULL)
		return ENOMEM;

	size_t offset = vaddr % PAGE_SIZE;
	memsize += offset;
	vaddr -= offset;

	size_t npages = (memsize + PAGE_SIZE - 1) / PAGE_SIZE;

	region->vaddr = vaddr;
	region->npages = npages;
	region->readable = readable;
	region->writeable = writeable;
	region->executable = executable;
	region->next = regions_list;

	as->regions = region;

	return 0;
}

int as_prepare_load(struct addrspace *as) {
	if (as == NULL)
		return ENOMEM;

	struct region* region = as->regions;
	while (region != NULL) {
		region->writeable_backup = region->writeable;
		region->writeable = 1;
		region = region->next;
	}

	return 0;
}

int as_complete_load(struct addrspace *as) {
	if (as == NULL)
		return ENOMEM;

	struct region* region = as->regions;
	while (region != NULL) {
		region->writeable = region->writeable_backup;
		region = region->next;
	}

	return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {
	if (as == NULL)
		return ENOMEM;

	as->stack_base = USERSPACETOP;
	as->stack_npages = USER_STACK_SIZE;
	*stackptr = USERSPACETOP;

	return 0;
}
