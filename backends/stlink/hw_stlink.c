/**
 * @file hw_stlink.c
 * @brief ST-Link backend implementation for the hardware abstraction layer.
 *
 * This file contains all the code necessary to communicate with a target
 * device using an ST-Link programmer via the libstlink library.
 * It provides a public registration function that makes its capabilities
 * available to the generic hw.c core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Include the ST-Link library's main header
#include <stlink/stlink.h>
#include <stlink/register.h>
#include <stlink/common_flash.h>
#include <stlink/flash_loader.h>
#include <read_write.h>

// Include the public hardware abstraction layer API header
#include "hw.h"

// --- Private Data Structure ---

/**
 * @brief Private data structure for the ST-Link backend.
 *
 * This struct holds all the state information needed for an active
 * ST-Link session, primarily the handle provided by libstlink.
 */
typedef struct {
    stlink_t *sl;
} hw_stlink_pvt_t;


// --- Forward declarations for the static implementation functions ---

/** These functions contain the actual logic for this backend. They are static
 * because they are only ever called via the 'stlink_ops' dispatch table.
 */
static hw_t* stlink_impl_connect(const char *host, int port);
static void stlink_impl_close(hw_t *ctx);
static int stlink_impl_write32(hw_t *ctx, unsigned int addr, unsigned int value);
static int stlink_impl_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out);
static int stlink_impl_board_halted(hw_t *ctx);
static int stlink_impl_board_halt(hw_t *ctx);
static int stlink_impl_board_step(hw_t *ctx);
static uint64_t stlink_impl_read_reg(hw_t *ctx, int reg);
static void stlink_impl_write_reg(hw_t *ctx, int reg, uint64_t val);
static int stlink_impl_board_run(hw_t *ctx);
static int stlink_impl_write8(hw_t *ctx, unsigned int addr, uint8_t value);
static int stlink_impl_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out);
static int stlink_impl_board_reset(hw_t *ctx);
static int stlink_impl_flash_info(hw_t *ctx, hw_flash_info_t *info_out);
static int stlink_impl_flash_sector(hw_t *ctx, unsigned int addr,
                                    unsigned int *sector_base, unsigned int *sector_size);
static int stlink_impl_flash_read(hw_t *ctx, unsigned int addr, uint8_t *buf, size_t len);
static int stlink_impl_flash_write(hw_t *ctx, unsigned int addr, const uint8_t *buf, size_t len);
static int stlink_impl_flash_erase(hw_t *ctx, unsigned int addr, size_t len);
static int stlink_impl_flash_mass_erase(hw_t *ctx);

// --- The Static Dispatch Table (vtable) ---

/**
 * @brief The operations table for the ST-Link backend.
 *
 * This struct maps the generic hardware operations (connect, close, etc.)
 * to the specific static functions within this file that implement them.
 * A pointer to this table is passed to the core library during registration.
 */
const hw_ops_t stlink_ops = {
    .connect = stlink_impl_connect,
    .close   = stlink_impl_close,
    .write32 = stlink_impl_write32,
    .write8 = stlink_impl_write8,

    .read32  = stlink_impl_read32,
    .read8   = stlink_impl_read8,
    .board_halted = stlink_impl_board_halted,
	.board_run = stlink_impl_board_run,
	.board_halt = stlink_impl_board_halt,
	.board_step = stlink_impl_board_step,
    .read_reg = stlink_impl_read_reg,
    .write_reg = stlink_impl_write_reg,
    .board_reset = stlink_impl_board_reset,
    .flash_info = stlink_impl_flash_info,
    .flash_sector = stlink_impl_flash_sector,
    .flash_read = stlink_impl_flash_read,
    .flash_write = stlink_impl_flash_write,
    .flash_erase = stlink_impl_flash_erase,
    .flash_mass_erase = stlink_impl_flash_mass_erase,
};


// --- Static Backend Function Implementations ---

/**
 * @brief The constructor function for the stlink backend.
 */
static hw_t* stlink_impl_connect(const char *host, int port) {
    // For stlink, host and port are unused as it connects via USB.
    (void)host;
    (void)port;

    // Allocate the private data structure for this backend instance
    hw_stlink_pvt_t *pvt = calloc(1, sizeof(hw_stlink_pvt_t));
    if (!pvt) {
        perror("Failed to allocate stlink private context");
        return NULL;
    }

    // Perform the actual connection using libstlink
    pvt->sl = stlink_open_usb(UERROR, CONNECT_UNDER_RESET, NULL, 0);
    if (!pvt->sl) {
        fprintf(stderr, "Failed to open ST-Link device\n");
        free(pvt);
        return NULL;
    }

    // Put the target into a known state (halted) for stable access
    if (stlink_force_debug(pvt->sl) != 0) {
        fprintf(stderr, "Failed to enter debug mode and halt core\n");
        stlink_close(pvt->sl);
        free(pvt);
        return NULL;
    }

    // Allocate the generic context struct that will be returned to the user
    hw_t *ctx = calloc(1, sizeof(hw_t));
    if (!ctx) {
        perror("Failed to allocate generic hw_context");
        stlink_close(pvt->sl);
        free(pvt);
        return NULL;
    }

    // The constructor's key job: wire up the vtable and private data pointers.
    ctx->ops = &stlink_ops;
    ctx->pvt_data = pvt;

    return ctx;
}

/**
 * @brief The destructor for the stlink backend.
 */
static void stlink_impl_close(hw_t *ctx) {
    if (ctx == NULL) return;

    // Get the private data from the generic context
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (pvt != NULL) {
        // Clean up the libstlink resources
        if (pvt->sl != NULL) {
            stlink_exit_debug_mode(pvt->sl);
            stlink_close(pvt->sl);
        }
        // Free the private data struct
        free(pvt);
    }
    // Free the generic context struct
    free(ctx);
}

/**
 * @brief The write32 implementation for the stlink backend.
 */
static int stlink_impl_write32(hw_t *ctx, unsigned int addr, unsigned int value) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return -1;

    // libstlink's API requires data to be placed in the internal buffer first.
    memcpy(pvt->sl->q_buf, &value, sizeof(unsigned int));
    if (stlink_write_mem32(pvt->sl, addr, sizeof(unsigned int)) != 0) {
        return -1;
    }
    return 0; // Success
}

static int stlink_impl_write8(hw_t *ctx, unsigned int addr, uint8_t value) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return -1;

    // Place the single byte into the buffer
    pvt->sl->q_buf[0] = value;

    // Write exactly 1 byte
    if (stlink_write_mem8(pvt->sl, addr, 1) != 0) {
        return -1;
    }
    return 0; // Success
}

static int stlink_impl_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl || !value_out) return -1;

    if (stlink_read_mem32(pvt->sl, addr & ~0x3u, sizeof(unsigned int)) != 0) {
        return -1;
    }

    *value_out = pvt->sl->q_buf[addr & 0x3u];
    return 0;
}

/**
 * @brief The read32 implementation for the stlink backend.
 */
static int stlink_impl_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl || !value_out) return -1;

    // Read data from the target into the internal buffer.
    if (stlink_read_mem32(pvt->sl, addr, sizeof(unsigned int)) != 0) {
        return -1;
    }
    // Copy the data from the internal buffer to the user's output variable.
    memcpy(value_out, pvt->sl->q_buf, sizeof(unsigned int));
    return 0; // Success
}

/**
 * @brief The board_halted implementation for the stlink backend
 */
static int stlink_impl_board_halted(hw_t *ctx) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return 0;

    stlink_status(pvt->sl);
    return pvt->sl->core_stat == TARGET_HALTED;
}

static int stlink_impl_board_run(hw_t *ctx) {
	hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
	if (!pvt || !pvt->sl) return -1;
	return stlink_run(pvt->sl, RUN_NORMAL);
}

/**
 * @brief The board_halt implementation for the stlink backend
 */
static int stlink_impl_board_halt(hw_t *ctx) {
	hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
	if (!pvt || !pvt->sl) return -1;

    if (stlink_force_debug(pvt->sl) != 0) {
        fprintf(stderr, "Failed to enter debug mode and halt core\n");
        return 1;
    }

    return 0; // Success
}

static int stlink_impl_board_step(hw_t *ctx) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return -1;

    if (stlink_step(pvt->sl) != 0) {
        fprintf(stderr, "Failed to single step\n");
        return 1;
    }

    return 0; // Success
}

/**
 * @brief The read_reg implementation for the stlink backend
 */
static uint64_t stlink_impl_read_reg(hw_t *ctx, int reg) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return 0;

    struct stlink_reg regp;
    if (stlink_read_reg(pvt->sl, reg, &regp) != 0) {
        fprintf(stderr, "stlink_read_reg failed\n");
        return 0;
    }

    if (reg >= 0 && reg < 16) {
        return regp.r[reg];
    }

    return 0;
}

/**
 * @brief The write_reg implementation for the stlink backend
 */
static void stlink_impl_write_reg(hw_t *ctx, int reg, uint64_t val) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return;

    stlink_write_reg(pvt->sl, (uint32_t)val, reg);
}

static int stlink_impl_board_reset(hw_t *ctx) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return -1;
    stlink_reset(pvt->sl, RESET_AUTO);
    return 0;
}

// --- Flash operations ---

/**
 * @brief The program granularity of the target's flash, in bytes.
 *
 * Erase granularity comes back from libstlink in sl->flash_pgsz, but program
 * granularity does not, and it varies by family: the newer parts can only be
 * written a doubleword at a time, and the H7 a whole 256-bit flash word.
 * hw_flash_patch() uses this to decide how wide a partial rewrite has to be.
 */
static unsigned int stlink_flash_write_granularity(stlink_t *sl) {
    switch (sl->flash_type) {
    case STM32_FLASH_TYPE_H7:
        return 32; // 256-bit flash word
    case STM32_FLASH_TYPE_G0:
    case STM32_FLASH_TYPE_G4:
    case STM32_FLASH_TYPE_L4:
    case STM32_FLASH_TYPE_L5_U5_H5:
    case STM32_FLASH_TYPE_WB_WL:
        return 8;  // doubleword
    case STM32_FLASH_TYPE_F0_F1_F3:
    case STM32_FLASH_TYPE_F1_XL:
        return 2;  // half-word
    default:
        return 4;
    }
}

static int stlink_impl_flash_info(hw_t *ctx, hw_flash_info_t *info_out) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl || !info_out) return -1;

    // These are all filled in by stlink_load_device_params() during connect.
    if (pvt->sl->flash_size == 0 || pvt->sl->flash_pgsz == 0) {
        fprintf(stderr, "ST-Link did not report a flash geometry for this chip.\n");
        return -1;
    }

    info_out->base = pvt->sl->flash_base;
    info_out->size = pvt->sl->flash_size;
    info_out->page_size = pvt->sl->flash_pgsz;
    info_out->write_align = stlink_flash_write_granularity(pvt->sl);
    return 0;
}

/**
 * @brief Look up a non-uniform erase sector by address.
 *
 * F2/F4 and F7 lay their banks out as a handful of small sectors, one medium
 * one, then large ones for the rest of the bank; sl->flash_pgsz is only a
 * nominal figure for them. Dual-bank parts repeat the pattern every 1 MB.
 *
 * @return 0 if this family has a non-uniform map and the address was resolved,
 *         -1 if the family is uniform (the caller should use flash_pgsz).
 */
static int stlink_flash_sector_map(stlink_t *sl, unsigned int addr,
                                   unsigned int *sector_base, unsigned int *sector_size) {
    // Sector sizes in KB, one bank's worth. Both tables total exactly 1 MB.
    static const uint32_t f2_f4_map[] = { 16, 16, 16, 16, 64, 128, 128, 128, 128, 128, 128, 128 };
    static const uint32_t f7_map[]    = { 32, 32, 32, 32, 128, 256, 256, 256 };

    const uint32_t *map;
    unsigned int entries;

    switch (sl->flash_type) {
    case STM32_FLASH_TYPE_F2_F4: map = f2_f4_map; entries = 12; break;
    case STM32_FLASH_TYPE_F7:    map = f7_map;    entries = 8;  break;
    default: return -1; // uniform layout
    }

    const uint32_t bank_size = 1024u * 1024u;
    uint32_t offset    = addr - sl->flash_base;
    uint32_t bank_base = (offset / bank_size) * bank_size;
    uint32_t in_bank   = offset - bank_base;

    uint32_t cursor = 0;
    for (unsigned int i = 0; i < entries; i++) {
        uint32_t size = map[i] * 1024u;
        if (in_bank < cursor + size) {
            *sector_base = sl->flash_base + bank_base + cursor;
            *sector_size = size;
            return 0;
        }
        cursor += size;
    }
    return -1;
}

static int stlink_impl_flash_sector(hw_t *ctx, unsigned int addr,
                                    unsigned int *sector_base, unsigned int *sector_size) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl || !sector_base || !sector_size) return -1;

    if (addr < pvt->sl->flash_base ||
        (addr - pvt->sl->flash_base) >= pvt->sl->flash_size) {
        return -1;
    }

    if (stlink_flash_sector_map(pvt->sl, addr, sector_base, sector_size) == 0) {
        return 0;
    }

    uint32_t pgsz = pvt->sl->flash_pgsz;
    if (pgsz == 0) return -1;
    *sector_base = pvt->sl->flash_base +
                   ((addr - pvt->sl->flash_base) / pgsz) * pgsz;
    *sector_size = pgsz;
    return 0;
}

// Read in word-aligned chunks that comfortably fit libstlink's q_buf.
#define STLINK_FLASH_READ_CHUNK 1024u

static int stlink_impl_flash_read(hw_t *ctx, unsigned int addr, uint8_t *buf, size_t len) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl || !buf) return -1;

    size_t done = 0;
    while (done < len) {
        // stlink_read_mem32 only accepts word-aligned addresses and whole
        // words, so read the enclosing aligned window and copy out the middle.
        unsigned int cur     = addr + (unsigned int)done;
        unsigned int aligned = cur & ~3u;
        unsigned int skip    = cur - aligned;

        size_t want  = len - done;
        size_t chunk = want + skip;
        if (chunk > STLINK_FLASH_READ_CHUNK) chunk = STLINK_FLASH_READ_CHUNK;
        chunk = (chunk + 3u) & ~(size_t)3u;

        if (stlink_read_mem32(pvt->sl, aligned, (uint16_t)chunk) != 0) {
            fprintf(stderr, "stlink_read_mem32 failed at 0x%08x\n", aligned);
            return -1;
        }

        size_t copy = chunk - skip;
        if (copy > want) copy = want;
        memcpy(buf + done, pvt->sl->q_buf + skip, copy);
        done += copy;
    }
    return 0;
}

static int stlink_impl_flash_write(hw_t *ctx, unsigned int addr, const uint8_t *buf, size_t len) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl || !buf) return -1;
    if (len == 0) return 0;

    // stlink_write_flash() erases every page it is about to touch, which is
    // exactly what hw_flash_patch() is trying to avoid. Drive the flash loader
    // directly instead, so that a write here is only ever a write.
    flash_loader_t fl;
    memset(&fl, 0, sizeof(fl));

    if (stlink_flashloader_start(pvt->sl, &fl) != 0) {
        fprintf(stderr, "Failed to start the ST-Link flash loader\n");
        return -1;
    }

    int rc = 0;
    if (stlink_flashloader_write(pvt->sl, &fl, addr,
                                 (uint8_t*)(uintptr_t)buf, (uint32_t)len) != 0) {
        fprintf(stderr, "Failed to program %zu bytes of flash at 0x%08x\n", len, addr);
        rc = -1;
    }

    // Always stop the loader, so the flash is left locked either way.
    if (stlink_flashloader_stop(pvt->sl, &fl) != 0) {
        fprintf(stderr, "Failed to stop the ST-Link flash loader\n");
        rc = -1;
    }
    return rc;
}

static int stlink_impl_flash_erase(hw_t *ctx, unsigned int addr, size_t len) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return -1;
    if (len == 0) return 0;

    if (stlink_erase_flash_section(pvt->sl, addr, (uint32_t)len, false) != 0) {
        fprintf(stderr, "Failed to erase %zu bytes of flash at 0x%08x\n", len, addr);
        return -1;
    }
    return 0;
}

static int stlink_impl_flash_mass_erase(hw_t *ctx) {
    hw_stlink_pvt_t *pvt = (hw_stlink_pvt_t*)ctx->pvt_data;
    if (!pvt || !pvt->sl) return -1;

    if (stlink_erase_flash_mass(pvt->sl) != 0) {
        fprintf(stderr, "Failed to mass erase flash\n");
        return -1;
    }
    return 0;
}
