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
#include <mips/trapframe.h>


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
				if (ch == '\r' || ch == '\n') {
					kernel_buf[i] = '\n';
					i++;
				}
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

int
sys_exit(int exitcode)
{
#if OPT_WAITPID
    struct proc *p = curproc;

    struct addrspace *as = proc_getas();
    proc_setas(NULL);
    as_deactivate();
    as_destroy(as);

    spinlock_acquire(&p->p_lock);
    p->p_exitcode = exitcode & 0xff;
    p->p_exited = true;
    spinlock_release(&p->p_lock);

	V(p->p_sem);
    proc_remthread(curthread);


#else
    struct addrspace *as = proc_getas();
    proc_setas(NULL);
    as_destroy(as);
    (void)exitcode;
#endif

    thread_exit();
    panic("thread_exit returned\n");
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
sys_waitpid(pid_t pid, userptr_t statusp, int options, int32_t *retval)
{
#if OPT_WAITPID
	struct proc *p;
	int status;
	int result;

	if (options != 0) {
		return EINVAL;
	}

	if (pid <= 0) {
		return ESRCH;
	}

	p = proc_search_pid(pid);
	if (p == NULL) {
		return ESRCH;
	}

	if (p->p_parent != curproc) {
#ifdef ECHILD
		return ECHILD;
#else
		return ESRCH;
#endif
	}

	spinlock_acquire(&p->p_lock);
	if (p->p_waited) {
		spinlock_release(&p->p_lock);
#ifdef ECHILD
		return ECHILD;
#else
		return ESRCH;
#endif
	}
	p->p_waited = true;
	spinlock_release(&p->p_lock);

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

static void
fork_thread_entry(void *tf, unsigned long junk)
{
        (void)junk;
        enter_forked_process((struct trapframe *)tf);
}


int
sys_fork(struct trapframe *tf, int32_t *retval)
{
	struct trapframe *child_tf;
	struct proc *child_proc;
	struct addrspace *child_as;
	int result;

	KASSERT(tf != NULL);
	KASSERT(retval != NULL);
	KASSERT(curproc != NULL);
	KASSERT(curproc->p_addrspace != NULL);

	child_tf = kmalloc(sizeof(struct trapframe));
	if (child_tf == NULL) {
		return ENOMEM;
	}
	*child_tf = *tf;

	child_proc = proc_create_runprogram(curproc->p_name);
	if (child_proc == NULL) {
		kfree(child_tf);
		return ENOMEM;
	}

	child_proc->p_parent = curproc;

	result = as_copy(curproc->p_addrspace, &child_as);
	if (result) {
		proc_destroy(child_proc);
		kfree(child_tf);
		return result;
	}

	spinlock_acquire(&child_proc->p_lock);
	child_proc->p_addrspace = child_as;
	spinlock_release(&child_proc->p_lock);

	result = thread_fork(curproc->p_name,
	                     child_proc,
	                     fork_thread_entry,
	                     child_tf,
	                     0);
	if (result) {
		child_proc->p_addrspace = NULL;
		as_destroy(child_as);
		proc_destroy(child_proc);
		kfree(child_tf);
		return result;
	}

	*retval = child_proc->p_pid;
	return 0;
}

int
sys_getpid(int32_t *retval)
{
	KASSERT(curproc != NULL);

	*retval = curproc->p_pid;
	return 0;
}