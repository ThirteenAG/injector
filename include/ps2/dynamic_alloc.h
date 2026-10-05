#ifndef WFP_MIPS_DYNAMIC_ALLOC_H
#define WFP_MIPS_DYNAMIC_ALLOC_H
#include "memalloc.h"
#include <stdint.h>

/* Backing allocation happens only when existing arenas cannot fit a request. */
#define MEM_HEADER_SIZE ((sizeof(struct mem_block) + 15u) & ~(size_t)15u)
struct mem_arena {
    struct mem_arena* next;
    void* allocation;
    int32_t token;
    struct mem_block* first;
};
#define MEM_ARENA_HEADER_SIZE ((sizeof(struct mem_arena) + 15u) & ~(size_t)15u)
static struct mem_arena* mem_arenas;

static void mem_split_block(struct mem_block* block, size_t size)
{
    struct mem_block* tail = (struct mem_block*)((char*)block + MEM_HEADER_SIZE + size);
    tail->size = block->size - size - MEM_HEADER_SIZE;
    tail->free = 1; tail->next = block->next;
    block->size = size; block->free = 0; block->next = tail;
}
static void mem_merge_blocks(struct mem_block* block)
{
    while (block && block->next) {
        if (block->free && block->next->free) {
            block->size += MEM_HEADER_SIZE + block->next->size;
            block->next = block->next->next;
        } else block = block->next;
    }
}
static void* mem_allocate_dynamic(size_t size)
{
    struct mem_arena* arena;
    struct mem_block* block;
    size_t bytes;
    int32_t token = 0;
    void* allocation;
    if (!size || size > SIZE_MAX - MEM_ARENA_HEADER_SIZE - MEM_HEADER_SIZE - 15) return NULL;
    size = (size + 15) & ~(size_t)15;
    for (arena = mem_arenas; arena; arena = arena->next) {
        for (block = arena->first; block; block = block->next) {
            if (!block->free || block->size < size) continue;
            if (block->size - size >= MEM_HEADER_SIZE + 16) mem_split_block(block, size);
            else block->free = 0;
            return (char*)block + MEM_HEADER_SIZE;
        }
    }
    bytes = size + MEM_ARENA_HEADER_SIZE + MEM_HEADER_SIZE;
    if (bytes < 4096) bytes = 4096;
    allocation = mem_backing_allocate(bytes, &token);
    /* A small allocation can still fit when the preferred 4 KiB arena cannot. */
    if (!allocation && bytes == 4096) {
        bytes = size + MEM_ARENA_HEADER_SIZE + MEM_HEADER_SIZE;
        allocation = mem_backing_allocate(bytes, &token);
    }
    if (!allocation) return NULL;
    if ((uintptr_t)allocation & 15) { mem_backing_release(allocation, token); return NULL; }
    memset(allocation, 0, bytes); /* Preserve zeroed fresh storage without CRT startup. */
    arena = (struct mem_arena*)allocation;
    arena->allocation = allocation; arena->token = token;
    arena->next = mem_arenas;
    arena->first = (struct mem_block*)((char*)allocation + MEM_ARENA_HEADER_SIZE);
    block = arena->first;
    block->size = bytes - MEM_ARENA_HEADER_SIZE - MEM_HEADER_SIZE;
    block->free = 1; block->next = NULL;
    mem_arenas = arena;
    if (block->size - size >= MEM_HEADER_SIZE + 16) mem_split_block(block, size);
    else block->free = 0;
    return (char*)block + MEM_HEADER_SIZE;
}
static void mem_free_dynamic(void* pointer)
{
    struct mem_arena** link = &mem_arenas;
    if (!pointer) return;
    while (*link) {
        struct mem_arena* arena = *link;
        struct mem_block* block;
        for (block = arena->first; block; block = block->next) {
            if ((char*)block + MEM_HEADER_SIZE != pointer) continue;
            if (!block->free) { block->free = 1; mem_merge_blocks(arena->first); }
            if (arena->first->free && !arena->first->next) {
                void* allocation = arena->allocation;
                int32_t token = arena->token;
                *link = arena->next;
                mem_backing_release(allocation, token);
            }
            return;
        }
        link = &arena->next;
    }
}
void mem_initialize(void) { /* Arenas are acquired lazily; never discard live blocks. */ }
void mem_split(struct mem_block* block, size_t size) { mem_split_block(block, size); }
void mem_merge(void)
{
    struct mem_arena* arena;
    for (arena = mem_arenas; arena; arena = arena->next) mem_merge_blocks(arena->first);
}
void* AllocMemBlock(size_t size) { return mem_allocate_dynamic(size); }
void FreeMemBlock(void* pointer) { mem_free_dynamic(pointer); }
#endif
