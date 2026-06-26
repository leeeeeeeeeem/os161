#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <vfs.h>
#include <vnode.h>
#include <stat.h>
#include <uio.h>
#include <bitmap.h>
#include <synch.h>
#include <swap.h>
#include <vm.h>

static struct vnode *swap_vnode = NULL;
static struct bitmap *swap_bitmap = NULL;
static struct lock *swap_lock = NULL;
static unsigned int swap_num_slots = 0;

void swap_bootstrap(void) {
	struct stat st;
	int err;

	swap_lock = lock_create("swap_lock");
	KASSERT(swap_lock != NULL);

	err = vfs_swapon("ldh1", &swap_vnode);
	if (err) {
		panic("Failed to open swap disk\n");
	}

	err = VOP_STAT(swap_vnode, &st);
	if (err) {
		panic("Error while calling VOP_STAT on swap disk\n");
	}

	swap_num_slots = st.st_size / PAGE_SIZE;

	swap_bitmap = bitmap_create(swap_num_slots);
	KASSERT(swap_bitmap != NULL);
}

int swap_alloc(unsigned *ret_slot) {
	int error;
	lock_acquire(swap_lock);
	error = bitmap_alloc(swap_bitmap, ret_slot);
	lock_release(swap_lock);
	return error;
}

void swap_free(unsigned slot) {
	lock_acquire(swap_lock);
	KASSERT(bitmap_isset(swap_bitmap, slot));
	bitmap_unmark(swap_bitmap, slot);
	lock_release(swap_lock);
}

int swap_write(vaddr_t kvaddr, unsigned slot) {
	struct iovec iov;
	struct uio u;
	int error;

	KASSERT(swap_vnode != NULL);
	KASSERT(slot < swap_num_slots);

	lock_acquire(swap_lock);
	uio_kinit(&iov, &u, (void*) kvaddr, PAGE_SIZE, (off_t) slot * PAGE_SIZE, UIO_WRITE);
	error = VOP_WRITE(swap_vnode, &u);
	if (error) {
		lock_release(swap_lock);
		return error;
	}
	if (u.uio_resid != 0) {
		lock_release(swap_lock);
		return ENOSPC;
	}
	lock_release(swap_lock);
	return 0;
}

int swap_read(vaddr_t kvaddr, unsigned slot) {
	struct iovec iov;
	struct uio u;
	int error;

	KASSERT(swap_vnode != NULL);
	KASSERT(slot < swap_num_slots);

	lock_acquire(swap_lock);
	uio_kinit(&iov, &u, (void*) kvaddr, PAGE_SIZE, (off_t) slot * PAGE_SIZE, UIO_READ);
	error = VOP_READ(swap_vnode, &u);
	if (error) {
		lock_release(swap_lock);
		return error;
	}
	if (u.uio_resid != 0) {
		lock_release(swap_lock);
		return EIO;
	}
	lock_release(swap_lock);
	return 0;
}
