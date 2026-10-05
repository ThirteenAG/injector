#ifndef H_MEMALLOC
#define H_MEMALLOC
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct mem_block {
    size_t size;
    int free;
    struct mem_block* next;
};

#ifdef __cplusplus
extern "C" {
#endif
void mem_initialize(void);
void mem_split(struct mem_block* fitting_slot, size_t size);
void* AllocMemBlock(size_t noOfBytes);
void mem_merge(void);
void FreeMemBlock(void* ptr);
uintptr_t psp_mem_storage_begin(void);
size_t psp_mem_storage_size(void);
#ifdef __cplusplus
}
#endif
#endif
