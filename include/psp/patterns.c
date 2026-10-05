#include "patterns.h"
#include "guest_memory.h"
#include <string.h>
#include <psputils.h>

void PatternSetGameBaseAddress(uintptr_t addr, size_t size)
{
    pattern.text_addr = addr;
    pattern.text_size = size;
}

/* Parse once into bounded stack storage. No VLA, mutation of string literals,
 * heap allocation, byte-sized loop counters, or short-range unsigned underflow. */
#define PATTERN_MAX_BYTES 256
static int pattern_hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int pattern_space(unsigned char c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static size_t pattern_parse(const char* text, uint8_t* bytes, uint8_t* mask)
{
    size_t count = 0;
    if (!text) return 0;
    while (*text) {
        int high, low;
        while (pattern_space((unsigned char)*text)) ++text;
        if (!*text) break;
        if (count == PATTERN_MAX_BYTES) return 0;
        if (*text == '?') {
            ++text; if (*text == '?') ++text;
            bytes[count] = 0; mask[count++] = 0;
        } else {
            high = pattern_hex((unsigned char)*text++);
            if (high < 0 || !*text) return 0;
            low = pattern_hex((unsigned char)*text++);
            if (low < 0) return 0;
            bytes[count] = (uint8_t)((high << 4) | low); mask[count++] = 255;
        }
    }
    return count;
}

uintptr_t range_get(size_t count, uintptr_t range_start, size_t range_size, const char* pattern_str, int32_t offset)
{
    uint8_t bytes[PATTERN_MAX_BYTES], mask[PATTERN_MAX_BYTES];
    size_t length = pattern_parse(pattern_str, bytes, mask), anchor = 0, i = 0, limit;
    const uint8_t* memory = (const uint8_t*)range_start;
    if (!length || !range_start || length > range_size || range_start > UINTPTR_MAX - range_size) return 0;
    limit = range_size - length;
    while (anchor < length && !mask[anchor]) ++anchor;
    /* Restore PPSSPP JIT markers before matching instruction signatures. */
    if (psp_guest_ram_range(range_start, range_size))
        sceKernelIcacheInvalidateRange(memory, (unsigned int)range_size);
    while (i <= limit) {
        size_t k;
        if (anchor < length) {
            const uint8_t* hit = (const uint8_t*)memchr(memory + i + anchor, bytes[anchor], limit - i + 1);
            if (!hit) return 0;
            i = (size_t)(hit - memory) - anchor;
        }
        for (k = 0; k < length; ++k)
            if (mask[k] && memory[i + k] != bytes[k]) break;
        if (k == length && !count--) {
            uintptr_t address = range_start + i;
            if (offset < 0) {
                uint32_t distance = 0u - (uint32_t)offset;
                return address >= distance ? address - distance : 0;
            }
            return address <= UINTPTR_MAX - (uint32_t)offset ? address + (uint32_t)offset : 0;
        }
        ++i;
    }
    return 0;
}

uintptr_t range_get_first(uintptr_t range_start, size_t range_size, const char* pattern_str, int32_t offset)
{
    return range_pattern.get(0, range_start, range_size, pattern_str, offset);
}

uintptr_t get(size_t count, const char* pattern_str, int32_t offset)
{
    return range_pattern.get(count, pattern.text_addr, pattern.text_size, pattern_str, offset);
}

uintptr_t get_first(const char* pattern_str, int32_t offset)
{
    return range_pattern.get_first(pattern.text_addr, pattern.text_size, pattern_str, offset);
}

struct range_pattern_t range_pattern =
{
    .get = range_get,
    .get_first = range_get_first
};

struct pattern_t pattern =
{
    .text_addr = 0,
    .text_size = 0,
    .get = get,
    .get_first = get_first,
    .SetGameBaseAddress = PatternSetGameBaseAddress
};
