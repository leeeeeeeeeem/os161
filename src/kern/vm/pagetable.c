#include <pagetable.h>
#include <kern/errno.h>
#include <addrspace.h>
#include <vm.h>

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

paddr_t pagetable_translate(struct pagedir* pt, vaddr_t vaddr) {
	uint32_t dir_idx = GET_DIR_INDEX(vaddr);
	uint32_t pt_idx = GET_PT_INDEX(vaddr);

	struct pagetable* pt_lv2 = pt->tables[dir_idx];
	if (pt_lv2 == NULL) {
		pt_lv2 = pagetable_create_lv2();
		if (pt_lv2 == NULL)
			return 0;
		pt->tables[dir_idx] = pt_lv2;
	}

	paddr_t entry = pt_lv2->entries[pt_idx];
	if (entry == 0) {
		vaddr_t tmp_vaddr = alloc_kpages(1);
		if (tmp_vaddr == 0)
			return 0;

		bzero((void*) tmp_vaddr, PAGE_SIZE);
		entry = KVADDR_TO_PADDR(tmp_vaddr);
		pt_lv2->entries[pt_idx] = entry;
	}
	
	return entry;
}

void pagetable_destroy(struct pagedir *pt) {
	struct pagetable* entry;

	for (int i = 0; i < PT_SIZE; i++) {
		entry = pt->tables[i];

		if (entry != NULL){
			for (int j = 0; j < PT_SIZE; j++) {
				paddr_t paddr = entry->entries[j];
				if (paddr != 0)
					free_kpages(PADDR_TO_KVADDR(paddr));
			}
			kfree(entry);
		}
	}
	kfree(pt);
}

struct pagedir* pagetable_copy(struct pagedir* pt) {
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
				paddr_t paddr = entry->entries[j];

				if (paddr != 0){
					vaddr_t new_vaddr = alloc_kpages(1);
					if (new_vaddr == 0){
						pagetable_destroy(new_pt);
						return NULL;
					}
					
					memcpy(
						(void*) new_vaddr, 
						(void*) PADDR_TO_KVADDR(paddr), 
						PAGE_SIZE
					);

					new_pt->tables[i]->entries[j] = KVADDR_TO_PADDR(new_vaddr);
				}
			}
		}
	}
	return new_pt;
}
