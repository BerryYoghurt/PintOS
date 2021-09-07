#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <debug.h>
#include <lib/stdbool.h>
#include "lib/user/syscall.h"
#include "threads/vaddr.h"

void syscall_init (void);

void syscall_halt (void) NO_RETURN;
void syscall_exit (int status) NO_RETURN;
pid_t syscall_exec (char *file);
int syscall_wait (pid_t);
bool syscall_create (char *file, unsigned initial_size);
bool syscall_remove (char *file);
int syscall_open (char *file);
int syscall_filesize (int fd);
int syscall_read (int fd, void *buffer, unsigned length);
int syscall_write (int fd, const void *buffer, unsigned length);
void syscall_seek (int fd, unsigned position);
unsigned syscall_tell (int fd);
void syscall_close (int fd);
mapid_t syscall_mmap (int fd, void *addr);
void syscall_munmap (mapid_t mapid);

#endif /* userprog/syscall.h */
