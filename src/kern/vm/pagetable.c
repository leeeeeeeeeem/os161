#include <pagetable.h>
#include <kern/errno.h>
#include <addrspace.h>
#include <vm.h>
#include <coremap.h>
#include <swap.h>
#include <vmstats.h>

struct pagedir* pagetable_create(void) {
	struct pagedir* dir = kmalloc(sizeof(struct pagedir));
	if (dir == NULL)
		return dir;

	for (int i = 0; i < PT_SIZE; i++) {
		dir->tables[i] = NULL;
	}

	return dir;
}

struct pagetable* pagetable_create_lv2(void) {
	struct pagetable* pt = kmalloc(sizeof(struct pagetable));
	if (pt == NULL)
		return pt;

	for (int i = 0; i < PT_SIZE; i++) {
		pt->entries[i] = 0;
	}

	return pt;
}

paddr_t pagetable_translate(struct addrspace* as, vaddr_t vaddr) {
	uint32_t dir_idx = GET_DIR_INDEX(vaddr);
	uint32_t pt_idx = GET_PT_INDEX(vaddr);
	struct pagedir* pt = as->pagetable;

	struct pagetable* pt_lv2 = pt->tables[dir_idx];
	if (pt_lv2 == NULL) {
		pt_lv2 = pagetable_create_lv2();
		if (pt_lv2 == NULL)
			return 0;
		pt->tables[dir_idx] = pt_lv2;
	}

	paddr_t entry = pt_lv2->entries[pt_idx];
	if (entry == 0) {
		vm_record_stat(STAT_PAGE_FAULT);
		vaddr_t tmp_vaddr = alloc_kpages(1);
		if (tmp_vaddr == 0)
			return 0;

		bzero((void*) tmp_vaddr, PAGE_SIZE);
		entry = KVADDR_TO_PADDR(tmp_vaddr) | PTE_PRESENT;
		pt_lv2->entries[pt_idx] = entry;
		coremap_set_owner(entry >> 12, as, vaddr);
	}
	else if (entry & PTE_SWAPPED) {
		vm_record_stat(STAT_PAGE_FAULT);
		vaddr_t tmp_vaddr = alloc_kpages(1);
		if (tmp_vaddr == 0)
			return 0;

		unsigned int swap_slot = entry >> 12;
		int err = swap_read(tmp_vaddr, swap_slot);
		if (err) {
			free_kpages(tmp_vaddr);
			return 0;
		}
		swap_free(swap_slot);

		entry = KVADDR_TO_PADDR(tmp_vaddr) | PTE_PRESENT;
		pt_lv2->entries[pt_idx] = entry;
		coremap_set_owner(entry >> 12, as, vaddr);
	}
	
	return entry;
}

void pagetable_destroy(struct pagedir *pt) {
	struct pagetable* entry;

	for (int i = 0; i < PT_SIZE; i++) {
		entry = pt->tables[i];

		if (entry != NULL){
			for (int j = 0; j < PT_SIZE; j++) {
				paddr_t pte = entry->entries[j];
				if (pte != 0 && (pte & PTE_SWAPPED) == 0) {
					free_kpages(PADDR_TO_KVADDR(pte & PAGE_FRAME));
				} else if (pte & PTE_SWAPPED) {
					swap_free(pte >> 12);
				}
			}
			kfree(entry);
		}
	}
	kfree(pt);
}

struct pagedir* pagetable_copy(struct addrspace *old, struct addrspace *newas) {
	struct pagedir* pt = old->pagetable;
	struct pagedir* new_pt = pagetable_create();
	if (new_pt == NULL)
		return NULL;

	struct pagetable* entry;
	for (int i = 0; i < PT_SIZE; i++) {
		entry = pt->tables[i];

		if (entry != NULL){
			new_pt->tables[i] = pagetable_create_lv2();
			if (new_pt->tables[i] == NULL){
				pagetable_destroy(new_pt);
				return NULL;
			}

			for (int j = 0; j < PT_SIZE; j++) {
				paddr_t pte = entry->entries[j];

				if (pte != 0 && (pte & PTE_SWAPPED) == 0){
					vaddr_t new_vaddr = alloc_kpages(1);
					if (new_vaddr == 0){
						pagetable_destroy(new_pt);
						return NULL;
					}
					
					memcpy(
						(void*) new_vaddr, 
						(void*) PADDR_TO_KVADDR(pte & PAGE_FRAME), 
						PAGE_SIZE
					);

					new_pt->tables[i]->entries[j] = KVADDR_TO_PADDR(new_vaddr) | PTE_PRESENT;
					
					uint32_t pframe = KVADDR_TO_PADDR(new_vaddr) / PAGE_SIZE;
					coremap_set_owner(pframe, newas, (i << 22) | (j << 12));
				}
				else if (pte & PTE_SWAPPED) {
					vaddr_t new_vaddr = alloc_kpages(1);
					if (new_vaddr == 0) {
						pagetable_destroy(new_pt);
						return NULL;
					}

					int err = swap_read(new_vaddr, pte >> 12);
					if (err) {
						free_kpages(new_vaddr);
						pagetable_destroy(new_pt);
						return NULL;
					}

					new_pt->tables[i]->entries[j] = KVADDR_TO_PADDR(new_vaddr) | PTE_PRESENT;
					
					uint32_t pframe = KVADDR_TO_PADDR(new_vaddr) / PAGE_SIZE;
					coremap_set_owner(pframe, newas, (i << 22) | (j << 12));
				}
			}
		}
	}
	return new_pt;
}

paddr_t *pagetable_get_entry(struct pagedir *pt, vaddr_t vaddr) {
	uint32_t dir_idx = GET_DIR_INDEX(vaddr);
	uint32_t pt_idx = GET_PT_INDEX(vaddr);

	struct pagetable* pt_lv2 = pt->tables[dir_idx];
	if (pt_lv2 == NULL) {
		pt_lv2 = pagetable_create_lv2();
		if (pt_lv2 == NULL)
			return NULL;
		pt->tables[dir_idx] = pt_lv2;
	}

	return &pt_lv2->entries[pt_idx];
}
