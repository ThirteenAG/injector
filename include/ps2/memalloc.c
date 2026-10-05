#include "memalloc.h"
#include <stdint.h>

#ifdef MEM_CUSTOM_TOTAL_SIZE
#if MEM_CUSTOM_TOTAL_SIZE < 64
#error MEM_CUSTOM_TOTAL_SIZE must reserve at least 64 bytes
#endif
#define MEM_HEADER_SIZE ((sizeof(struct mem_block) + 15u) & ~(size_t)15u)

int mem_custom_initialized = 0;
#if defined(_MSC_VER)
__declspec(align(16)) char mem_custom[MEM_CUSTOM_TOTAL_SIZE] = { 1 };
#else
char mem_custom[MEM_CUSTOM_TOTAL_SIZE] __attribute__((aligned(16))) = { 1 };
#endif
struct mem_block* mem_freeList = (struct mem_block*)mem_custom;

void mem_initialize(void)
{
    if (mem_custom_initialized) return;
    memset(mem_custom, 0, sizeof(mem_custom));
    mem_freeList->size = (sizeof(mem_custom) - MEM_HEADER_SIZE) & ~(size_t)15;
    mem_freeList->free = 1;
    mem_freeList->next = NULL;
    mem_custom_initialized = 1;
}

void mem_split(struct mem_block* block, size_t size)
{
    struct mem_block* tail = (struct mem_block*)((char*)block + MEM_HEADER_SIZE + size);
    tail->size = block->size - size - MEM_HEADER_SIZE;
    tail->free = 1;
    tail->next = block->next;
    block->size = size;
    block->free = 0;
    block->next = tail;
}

void* AllocMemBlock(size_t size)
{
    struct mem_block* block;
    if (!size || size > SIZE_MAX - 15) return NULL;
    size = (size + 15) & ~(size_t)15;
    if (!mem_custom_initialized) mem_initialize();
    for (block = mem_freeList; block; block = block->next) {
        if (!block->free || block->size < size) continue;
        if (block->size - size >= MEM_HEADER_SIZE + 16) mem_split(block, size);
        else block->free = 0;
        return (char*)block + MEM_HEADER_SIZE;
    }
    return NULL;
}

void mem_merge(void)
{
    struct mem_block* block = mem_freeList;
    while (block && block->next) {
        if (block->free && block->next->free) {
            block->size += MEM_HEADER_SIZE + block->next->size;
            block->next = block->next->next;
        } else block = block->next;
    }
}

void FreeMemBlock(void* pointer)
{
    struct mem_block* block;
    if (!pointer || !mem_custom_initialized) return;
    /* Search for an exact payload address; never trust an arbitrary header. */
    for (block = mem_freeList; block; block = block->next) {
        if ((char*)block + MEM_HEADER_SIZE != pointer) continue;
        if (!block->free) { block->free = 1; mem_merge(); }
        return;
    }
}
#else
/* malloc is the initialized PRIVATE module heap from ps2sdk/plugins/module-runtime.c. */
#include <stdlib.h>
static void* mem_backing_allocate(size_t size, int32_t* token)
{ *token = 0; return malloc(size); }
static void mem_backing_release(void* allocation, int32_t token)
{ (void)token; free(allocation); }
#include "dynamic_alloc.h"
#endif
