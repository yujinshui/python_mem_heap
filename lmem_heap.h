#ifndef LMEM_HEAP_H
#define LMEM_HEAP_H


void* lmem_malloc(size_t nbytes);
void lmem_free(void* p);
void * lmem_relocate(void *p, size_t size);


#endif
