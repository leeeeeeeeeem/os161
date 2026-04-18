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
	char kernel_buf[size + 1];
	int result;

	if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
		return EBADF;
	}

	result = copyin(buf_ptr, kernel_buf, size);
	if (result) {
		return result;
	}

	for (size_t i = 0; i < size; i++){
		putch(kernel_buf[i]);
	}

	*retval = (int32_t)size;
	return 0;
}


int sys_read(int fd, userptr_t buf_ptr, size_t size, int32_t *retval){
	char kernel_buf[size];
	int result;

	if (fd != STDIN_FILENO) {
		return EBADF;
	}

	for (size_t i = 0; i < size; i++){
		kernel_buf[i] = getch();
		if (kernel_buf[i] == -1 || kernel_buf[i] == '\r' || kernel_buf[i] == '\n') {
			*retval = (int32_t)i;
			goto done;
		}
	}

	*retval = (int32_t)size;

done:
	result = copyout(kernel_buf, buf_ptr, *retval);
	if (result) {
		return result;
	}

	return 0;
}

int sys_exit(int exitcode){
	curproc->p_exitcode = exitcode;
	curproc->p_exited = true;

	thread_exit();

	panic("sys_exit: thread_exit returned\n");
	return 0;
}
