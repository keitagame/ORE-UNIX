#include "memory.h"

#define PAGE_SIZE 4096
#define MAX_PAGES 32768   // 128MB 分のページ数

static uint8_t bitmap[MAX_PAGES / 8];

static inline void bitmap_set(int idx) {
    bitmap[idx / 8] |= (1 << (idx % 8));
}

static inline void bitmap_clear(int idx) {
    bitmap[idx / 8] &= ~(1 << (idx % 8));
}

static inline int bitmap_test(int idx) {
    return bitmap[idx / 8] & (1 << (idx % 8));
}

static uint32_t total_pages = 0;

void memory_init(uint32_t mem_lower, uint32_t mem_upper) {
    // mem_lower: 1MB 未満
    // mem_upper: 1MB 以上の KB 単位のサイズ
    uint32_t total_kb = mem_lower + mem_upper;
    total_pages = (total_kb * 1024) / PAGE_SIZE;

    // 全ページを free に
    for (uint32_t i = 0; i < total_pages / 8; i++) {
        bitmap[i] = 0;
    }

    // 最初の 1MB は予約扱い
    for (uint32_t addr = 0; addr < 0x100000; addr += PAGE_SIZE) {
        bitmap_set(addr / PAGE_SIZE);
    }
}

void* alloc_page(void) {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (void*)(i * PAGE_SIZE);
        }
    }
    return 0; // メモリ不足
}

void free_page(void* addr) {
    uint32_t idx = (uint32_t)addr / PAGE_SIZE;
    bitmap_clear(idx);
}
