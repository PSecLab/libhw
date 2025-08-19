#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// This now includes the private header from the same 'core' directory
// to get the definition of 'backend_node_t'.
#include "hw_priv.h"

/**
 * Connects to a hardware debugger by iterating the private, internal list.
 */
hw_t* hw_connect(const char *backend_name, const char *host, int port) {
	// A 'for' loop is the standard C idiom for iterating a known array.
    // The loop continues until it finds the entry where the name is NULL.
    for (int i = 0; g_known_backends[i].name != NULL; ++i) {
        const backend_entry_t* backend = &g_known_backends[i];

        // Compare the requested name with the current backend's name.
        if (strcmp(backend_name, backend->name) == 0) {
            // Found a match. Call the 'connect' function from its ops table.
            if (backend->ops && backend->ops->connect) {
                return backend->ops->connect(host, port);
            }
        }
    }

    // If the loop finishes without finding a match, the backend is not available.
    fprintf(stderr, "Error: Backend '%s' not found in the central registry.\n", backend_name);
    return NULL;
}

/**
 * Generic wrapper for the close operation.
 */
void hw_close(hw_t *ctx) {
    if (ctx && ctx->ops && ctx->ops->close) {
        ctx->ops->close(ctx);
    }
}

/**
 * Generic wrapper for the write32 operation.
 */
int hw_write32(hw_t *ctx, unsigned int addr, unsigned int value) {
    if (ctx && ctx->ops && ctx->ops->write32) {
        return ctx->ops->write32(ctx, addr, value);
    }
    return -1;
}

/**
 * Generic wrapper for the read32 operation.
 */
int hw_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out) {
    if (ctx && ctx->ops && ctx->ops->read32) {
        return ctx->ops->read32(ctx, addr, value_out);
    }
    return -1;
}

/**
 * @brief Generic wrapper to check if the board is halted.
 */
int hw_board_halted(hw_t *ctx) {
    if (ctx && ctx->ops && ctx->ops->board_halted) {
        return ctx->ops->board_halted(ctx);
    }
    return 0; // Return not halted on error
}

int hw_board_run(hw_t *ctx) {
    if (ctx && ctx->ops && ctx->ops->board_run) {
        return ctx->ops->board_run(ctx);
    }
    return 0; // Return not halted on error
}

/**
 * @brief Generic wrapper to read a register.
 */
uint64_t hw_read_reg(hw_t *ctx, int reg) {
    if (ctx && ctx->ops && ctx->ops->read_reg) {
        return ctx->ops->read_reg(ctx, reg);
    }
    return 0; // Return 0 on error
}

/**
 * @brief Generic wrapper to write to a register.
 */
void hw_write_reg(hw_t *ctx, int reg, uint64_t val) {
    if (ctx && ctx->ops && ctx->ops->write_reg) {
        ctx->ops->write_reg(ctx, reg, val);
    }
}
