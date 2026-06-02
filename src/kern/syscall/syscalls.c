#include "addrspace.h"
#include <types.h>
#include <kern/errno.h>
#include <kern/reboot.h>
#include <kern/unistd.h>
#include <lib.h>
#include <uio.h>
#include <clock.h>
#include <mainbus.h>
#include <synch.h>
#include <thread.h>
#include <proc.h>
#include <vfs.h>
#include <sfs.h>
#include <syscall.h>
#include <test.h>
#include <current.h>
#include <copyinout.h>

int sys_write(int fd, userptr_t buf_ptr, size_t size, int32_t *retval){
	char kernel_buf[64];
	size_t nbytes;
	int result;

	if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
		return EBADF;
	}

	for (size_t i = 0; i < size; i += nbytes) {
		nbytes = size - i;
		if (nbytes > sizeof(kernel_buf)) {
			nbytes = sizeof(kernel_buf);
		}

		result = copyin(buf_ptr + i, kernel_buf, nbytes);
		if (result) {
			return result;
		}

		for (size_t j = 0; j < nbytes; j++) {
			putch(kernel_buf[j]);
		}
	}

	*retval = (int32_t)size;
	return 0;
}


int sys_read(int fd, userptr_t buf_ptr, size_t size, int32_t *retval){
	char kernel_buf[64];
	size_t nread = 0;
	int result;

	if (fd != STDIN_FILENO) {
		return EBADF;
	}

	while (nread < size) {
		size_t nbytes = size - nread;
		if (nbytes > sizeof(kernel_buf)) {
			nbytes = sizeof(kernel_buf);
		}

		size_t i;
		for (i = 0; i < nbytes; i++) {
			int ch = getch();
			if (ch == -1 || ch == '\r' || ch == '\n') {
				if (i > 0) {
					result = copyout(kernel_buf, buf_ptr + nread, i);
					if (result) {
						return result;
					}
					nread += i;
				}
				*retval = (int32_t)nread;
				return 0;
			}
			kernel_buf[i] = (char)ch;
		}

		result = copyout(kernel_buf, buf_ptr + nread, nbytes);
		if (result) {
			return result;
		}
		nread += nbytes;
	}

	*retval = (int32_t)nread;
	return 0;
}

void
sys__exit(int status)
{
#if OPT_WAITPID
	struct proc *p = curproc;

	spinlock_acquire(&p->p_lock);
	p->p_exitcode = status & 0xff;
	p->p_exited = true;
	spinlock_release(&p->p_lock);

	/*
	 * Ordine fondamentale:
	 * prima rimuovi il thread dalla proc,
	 * poi svegli chi sta facendo proc_wait.
	 */
	proc_remthread(curthread);

	V(p->p_sem);

#else
	struct addrspace *as = proc_getas();
	as_destroy(as);
#endif

	thread_exit();

	panic("thread_exit returned (should not happen)\n");
}

int sys_sbrk(intptr_t amount, vaddr_t *retval) {
	struct addrspace *as = proc_getas();
	if (as == NULL) {
		return EFAULT;
	}

	vaddr_t old_heap_end = as->heap_end;
	vaddr_t new_heap_end = old_heap_end + amount;

	if (new_heap_end < as->heap_start) {
		return EINVAL;
	}

	vaddr_t stack_limit = as->stack_base - (as->stack_npages * PAGE_SIZE);
	if (new_heap_end >= stack_limit) {
		return ENOMEM;
	}

	as->heap_end = new_heap_end;
	*retval = old_heap_end;
	return 0;
}

int
sys_waitpid(pid_t pid, userptr_t statusp, int options, pid_t *retval)
{
#if OPT_WAITPID
	struct proc *p;
	int status;
	int result;

	if (options != 0) {
		return EINVAL;
	}

	p = proc_search_pid(pid);
	if (p == NULL) {
		return ESRCH;
	}

	status = proc_wait(p);

	if (statusp != NULL) {
		result = copyout(&status, statusp, sizeof(int));
		if (result) {
			return result;
		}
	}

	*retval = pid;
	return 0;
#else
	(void)pid;
	(void)statusp;
	(void)options;
	(void)retval;

	return ENOSYS;
#endif
}
