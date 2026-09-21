/**
 * @file hw_mock.c
 * @brief In-memory mock backend implementation for libhw testing and simulation.
 *
 * The flash window is modelled with real NOR semantics: erasing sets bytes to
 * 0xFF and programming can only clear bits, never set them. That is what makes
 * this backend useful as a test double -- code that forgets to erase, or that
 * assumes a write can raise a bit, fails here exactly as it would on a board.
 * Plain write8/write32 into the flash window follow the same rule.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hw.h"

#define MAX_MEM_PAGES 256
#define PAGE_SIZE 4096

/* A plausible small STM32: 256 KB of flash in uniform 1 KB pages. */
#define MOCK_FLASH_BASE  0x08000000u
#define MOCK_FLASH_SIZE  (256u * 1024u)
#define MOCK_FLASH_PAGE  1024u
#define MOCK_FLASH_ALIGN 4u

#define MOCK_FLASH_ERASED_BYTE 0xFF

typedef struct {
    uint32_t page_addr;
    uint8_t data[PAGE_SIZE];
} mem_page_t;

typedef struct {
    uint64_t regs[32];
    int is_halted;
    mem_page_t pages[MAX_MEM_PAGES];
    size_t num_pages;
    uint8_t flash[MOCK_FLASH_SIZE];
} hw_mock_pvt_t;

/** True if 'addr' falls inside the emulated flash window. */
static int mock_in_flash(unsigned int addr) {
    return addr >= MOCK_FLASH_BASE && (addr - MOCK_FLASH_BASE) < MOCK_FLASH_SIZE;
}

/** True if all of [addr, addr + len) falls inside the emulated flash window. */
static int mock_flash_range_ok(unsigned int addr, size_t len) {
    if (len == 0 || len > MOCK_FLASH_SIZE) return 0;
    if (addr < MOCK_FLASH_BASE) return 0;
    return (addr - MOCK_FLASH_BASE) <= MOCK_FLASH_SIZE - (unsigned int)len;
}

static mem_page_t* get_or_create_page(hw_mock_pvt_t *pvt, uint32_t addr, int create) {
    uint32_t page_addr = addr & ~(PAGE_SIZE - 1);
    for (size_t i = 0; i < pvt->num_pages; i++) {
        if (pvt->pages[i].page_addr == page_addr) {
            return &pvt->pages[i];
        }
    }
    if (!create || pvt->num_pages >= MAX_MEM_PAGES) return NULL;
    mem_page_t *p = &pvt->pages[pvt->num_pages++];
    p->page_addr = page_addr;
    memset(p->data, 0, PAGE_SIZE);
    return p;
}

static hw_t* mock_impl_connect(const char *host, int port) {
    (void)host; (void)port;
    hw_mock_pvt_t *pvt = calloc(1, sizeof(hw_mock_pvt_t));
    if (!pvt) return NULL;
    pvt->is_halted = 1;
    memset(pvt->flash, MOCK_FLASH_ERASED_BYTE, sizeof(pvt->flash));
    hw_t *ctx = calloc(1, sizeof(hw_t));
    if (!ctx) { free(pvt); return NULL; }
    extern const hw_ops_t mock_ops;
    ctx->ops = &mock_ops;
    ctx->pvt_data = pvt;
    return ctx;
}

static void mock_impl_close(hw_t *ctx) {
    if (!ctx) return;
    if (ctx->pvt_data) free(ctx->pvt_data);
    free(ctx);
}

static int mock_impl_write8(hw_t *ctx, unsigned int addr, uint8_t value) {
    if (!ctx || !ctx->pvt_data) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    if (mock_in_flash(addr)) {
        // NOR flash: programming clears bits, it cannot set them.
        pvt->flash[addr - MOCK_FLASH_BASE] &= value;
        return 0;
    }
    mem_page_t *p = get_or_create_page(pvt, addr, 1);
    if (!p) return -1;
    p->data[addr & (PAGE_SIZE - 1)] = value;
    return 0;
}

static int mock_impl_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out) {
    if (!ctx || !ctx->pvt_data || !value_out) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    if (mock_in_flash(addr)) {
        *value_out = pvt->flash[addr - MOCK_FLASH_BASE];
        return 0;
    }
    mem_page_t *p = get_or_create_page(pvt, addr, 0);
    if (!p) {
        *value_out = 0;
    } else {
        *value_out = p->data[addr & (PAGE_SIZE - 1)];
    }
    return 0;
}

static int mock_impl_write32(hw_t *ctx, unsigned int addr, unsigned int value) {
    uint8_t b0 = (uint8_t)(value & 0xFF);
    uint8_t b1 = (uint8_t)((value >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)((value >> 16) & 0xFF);
    uint8_t b3 = (uint8_t)((value >> 24) & 0xFF);
    if (mock_impl_write8(ctx, addr, b0) != 0) return -1;
    if (mock_impl_write8(ctx, addr + 1, b1) != 0) return -1;
    if (mock_impl_write8(ctx, addr + 2, b2) != 0) return -1;
    if (mock_impl_write8(ctx, addr + 3, b3) != 0) return -1;
    return 0;
}

static int mock_impl_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out) {
    if (!value_out) return -1;
    uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    mock_impl_read8(ctx, addr, &b0);
    mock_impl_read8(ctx, addr + 1, &b1);
    mock_impl_read8(ctx, addr + 2, &b2);
    mock_impl_read8(ctx, addr + 3, &b3);
    *value_out = ((uint32_t)b0) | (((uint32_t)b1) << 8) | (((uint32_t)b2) << 16) | (((uint32_t)b3) << 24);
    return 0;
}

static int mock_impl_board_halted(hw_t *ctx) {
    if (!ctx || !ctx->pvt_data) return 0;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    return pvt->is_halted;
}

static int mock_impl_board_run(hw_t *ctx) {
    if (!ctx || !ctx->pvt_data) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    pvt->is_halted = 0;
    return 0;
}

static int mock_impl_board_halt(hw_t *ctx) {
    if (!ctx || !ctx->pvt_data) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    pvt->is_halted = 1;
    return 0;
}

static int mock_impl_board_step(hw_t *ctx) {
    if (!ctx || !ctx->pvt_data) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    pvt->is_halted = 1;
    return 0;
}

static uint64_t mock_impl_read_reg(hw_t *ctx, int reg) {
    if (!ctx || !ctx->pvt_data || reg < 0 || reg >= 32) return 0;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    return pvt->regs[reg];
}

static void mock_impl_write_reg(hw_t *ctx, int reg, uint64_t val) {
    if (!ctx || !ctx->pvt_data || reg < 0 || reg >= 32) return;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    pvt->regs[reg] = val;
}

static int mock_impl_board_reset(hw_t *ctx) {
    if (!ctx || !ctx->pvt_data) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    memset(pvt->regs, 0, sizeof(pvt->regs));
    pvt->is_halted = 1;
    return 0;
}

static int mock_impl_flash_info(hw_t *ctx, hw_flash_info_t *info_out) {
    if (!ctx || !ctx->pvt_data || !info_out) return -1;
    info_out->base = MOCK_FLASH_BASE;
    info_out->size = MOCK_FLASH_SIZE;
    info_out->page_size = MOCK_FLASH_PAGE;
    info_out->write_align = MOCK_FLASH_ALIGN;
    return 0;
}

/* No flash_sector op: the layout is uniform, so the core's own page-size
 * fallback is exactly right, and leaving it NULL keeps that path exercised. */

static int mock_impl_flash_read(hw_t *ctx, unsigned int addr, uint8_t *buf, size_t len) {
    if (!ctx || !ctx->pvt_data || !buf) return -1;
    if (len == 0) return 0;
    if (!mock_flash_range_ok(addr, len)) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    memcpy(buf, pvt->flash + (addr - MOCK_FLASH_BASE), len);
    return 0;
}

static int mock_impl_flash_write(hw_t *ctx, unsigned int addr, const uint8_t *buf, size_t len) {
    if (!ctx || !ctx->pvt_data || !buf) return -1;
    if (len == 0) return 0;
    if (!mock_flash_range_ok(addr, len)) return -1;

    // Hold callers to the part's program granularity, the way real hardware
    // would, so that misaligned writes surface here rather than on a board.
    if ((addr % MOCK_FLASH_ALIGN) != 0 || (len % MOCK_FLASH_ALIGN) != 0) {
        fprintf(stderr, "mock: flash write at 0x%08x of %zu bytes is not %u-byte aligned\n",
                addr, len, MOCK_FLASH_ALIGN);
        return -1;
    }

    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    uint8_t *dst = pvt->flash + (addr - MOCK_FLASH_BASE);
    for (size_t i = 0; i < len; i++) {
        dst[i] &= buf[i];
    }
    return 0;
}

static int mock_impl_flash_erase(hw_t *ctx, unsigned int addr, size_t len) {
    if (!ctx || !ctx->pvt_data) return -1;
    if (len == 0) return 0;
    if (!mock_flash_range_ok(addr, len)) return -1;

    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;

    // Erase works on whole pages, so widen the range out to page boundaries.
    unsigned int first = ((addr - MOCK_FLASH_BASE) / MOCK_FLASH_PAGE) * MOCK_FLASH_PAGE;
    unsigned int last  = ((addr - MOCK_FLASH_BASE + (unsigned int)len - 1) / MOCK_FLASH_PAGE + 1)
                         * MOCK_FLASH_PAGE;
    if (last > MOCK_FLASH_SIZE) last = MOCK_FLASH_SIZE;

    memset(pvt->flash + first, MOCK_FLASH_ERASED_BYTE, last - first);
    return 0;
}

static int mock_impl_flash_mass_erase(hw_t *ctx) {
    if (!ctx || !ctx->pvt_data) return -1;
    hw_mock_pvt_t *pvt = (hw_mock_pvt_t*)ctx->pvt_data;
    memset(pvt->flash, MOCK_FLASH_ERASED_BYTE, sizeof(pvt->flash));
    return 0;
}

const hw_ops_t mock_ops = {
    .connect = mock_impl_connect,
    .close   = mock_impl_close,
    .write32 = mock_impl_write32,
    .read32  = mock_impl_read32,
    .write8  = mock_impl_write8,
    .read8   = mock_impl_read8,
    .board_halted = mock_impl_board_halted,
    .board_run    = mock_impl_board_run,
    .board_halt   = mock_impl_board_halt,
    .board_step   = mock_impl_board_step,
    .read_reg  = mock_impl_read_reg,
    .write_reg = mock_impl_write_reg,
    .board_reset = mock_impl_board_reset,
    .flash_info = mock_impl_flash_info,
    .flash_read = mock_impl_flash_read,
    .flash_write = mock_impl_flash_write,
    .flash_erase = mock_impl_flash_erase,
    .flash_mass_erase = mock_impl_flash_mass_erase,
};
