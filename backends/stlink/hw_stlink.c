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

// Include the ST-Link library's main header
#include <stlink/stlink.h>
#include <stlink/register.h>
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
