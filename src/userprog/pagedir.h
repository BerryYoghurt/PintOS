#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

#include <stdbool.h>
#include <stdint.h>

uint32_t *pagedir_create (void);
void pagedir_destroy (uint32_t *pd, uint32_t **supp_padedir);

bool pagedir_set_page (uint32_t *pd, void *upage,bool rw, bool file, uint32_t info);
void *pagedir_get_page (uint32_t *pd, const void *upage);
void pagedir_clear_page (uint32_t *pd, void *upage);

bool pagedir_is_dirty (uint32_t *pd, const void *upage);
void pagedir_set_dirty (uint32_t *pd, const void *upage, bool dirty);

bool pagedir_is_accessed (uint32_t *pd, const void *upage);
void pagedir_set_accessed (uint32_t *pd, const void *upage, bool accessed);

bool pagedir_is_mapped (uint32_t *pd, const void *upage);
void pagedir_set_unmapped (uint32_t *pd, const void *upage);

bool pagedir_is_writable (uint32_t *pd, const void *upage);

bool pagedir_is_filesys (uint32_t *pd, const void *upage);
void pagedir_set_swappable (uint32_t *pd, const void *upage);

void pagedir_set_present (uint32_t *pd, const void *upage, const void *kpage);
bool pagedir_is_present (uint32_t *pd, const void *upage);

uint32_t pagedir_get_supp (uint32_t *pd, const void *upage);
void pagedir_set_supp (uint32_t *pd, const void *upage, uint32_t supp_idx);

void pagedir_activate (uint32_t *pd);

/* For debugging */
bool pagedir_is_destoryed (void);

#endif /* userprog/pagedir.h */
