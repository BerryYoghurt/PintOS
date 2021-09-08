#ifndef VM_MMAP_H
#define VM_MMAP_H

struct mapping
{
  struct list_elem elem;
  struct file *file;
  void *addr;
  int id;
};

#endif