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

int hw_write8(hw_t *ctx, unsigned int addr, uint8_t value) {
    if (ctx && ctx->ops && ctx->ops->write8) {
        return ctx->ops->write8(ctx, addr, value);
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

int hw_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out) {
    if (ctx && ctx->ops && ctx->ops->read8) {
        return ctx->ops->read8(ctx, addr, value_out);
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

int hw_board_halt(hw_t *ctx) {
    if (ctx && ctx->ops && ctx->ops->board_halt) {
        return ctx->ops->board_halt(ctx);
    }
    return 0; // Return not halted on error
}

int hw_board_step(hw_t *ctx) {
    if (ctx && ctx->ops && ctx->ops->board_step) {
        return ctx->ops->board_step(ctx);
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

int hw_board_reset(hw_t *ctx) {
    if (ctx && ctx->ops && ctx->ops->board_reset) {
        return ctx->ops->board_reset(ctx);
    }
    return 0;
}

/* ==========================================================================
 *                                  Flash
 *
 * The backends supply five primitives (info, sector, read, write, erase) and
 * the two composite operations below -- update and patch -- are built once,
 * here, on top of them. Anything a backend leaves NULL either falls back to a
 * generic implementation (read) or makes the wrapper fail (everything else).
 * ========================================================================== */

/** Reject ranges that fall outside the flash, and zero-length requests. */
static int flash_range_ok(const hw_flash_info_t *info, unsigned int addr, size_t len) {
    if (len == 0 || len > (size_t)info->size) return -1;
    if (addr < info->base) return -1;
    if ((addr - info->base) > info->size - (unsigned int)len) return -1;
    return 0;
}

/**
 * Programming flash while the core is executing out of it is not safe, so the
 * composite operations halt first. They deliberately leave the target halted:
 * resuming a core whose code just changed underneath it is the caller's call.
 */
static int flash_halt_target(hw_t *ctx) {
    if (hw_board_halted(ctx)) return 0;
    if (hw_board_halt(ctx) != 0) {
        fprintf(stderr, "Failed to halt the target before programming flash.\n");
        return -1;
    }
    return 0;
}

int hw_flash_info(hw_t *ctx, hw_flash_info_t *info_out) {
    if (!ctx || !ctx->ops || !ctx->ops->flash_info || !info_out) return -1;

    memset(info_out, 0, sizeof(*info_out));
    if (ctx->ops->flash_info(ctx, info_out) != 0) return -1;

    // A zero erase granularity would make every size calculation below
    // degenerate, so treat it as a backend that does not really know.
    if (info_out->page_size == 0) return -1;
    if (info_out->write_align == 0) info_out->write_align = 1;
    return 0;
}

int hw_flash_sector(hw_t *ctx, unsigned int addr,
                    unsigned int *sector_base, unsigned int *sector_size) {
    if (!ctx || !ctx->ops || !sector_base || !sector_size) return -1;

    if (ctx->ops->flash_sector) {
        return ctx->ops->flash_sector(ctx, addr, sector_base, sector_size);
    }

    // No sector map from the backend: assume the layout is uniform.
    hw_flash_info_t info;
    if (hw_flash_info(ctx, &info) != 0) return -1;
    if (flash_range_ok(&info, addr, 1) != 0) return -1;

    *sector_base = info.base + ((addr - info.base) / info.page_size) * info.page_size;
    *sector_size = info.page_size;
    return 0;
}

int hw_flash_read(hw_t *ctx, unsigned int addr, void *buf, size_t len) {
    if (!ctx || !ctx->ops || !buf) return -1;
    if (len == 0) return 0;

    if (ctx->ops->flash_read) {
        return ctx->ops->flash_read(ctx, addr, (uint8_t*)buf, len);
    }

    // Fallback: flash is memory mapped on these parts, so ordinary loads work.
    // Use read32 on the aligned body and read8 for the ragged ends.
    uint8_t *out = (uint8_t*)buf;
    size_t i = 0;
    while (i < len) {
        unsigned int a = addr + (unsigned int)i;
        if ((a & 3u) == 0 && (len - i) >= 4 && ctx->ops->read32) {
            unsigned int w = 0;
            if (ctx->ops->read32(ctx, a, &w) != 0) return -1;
            out[i + 0] = (uint8_t)(w & 0xFF);
            out[i + 1] = (uint8_t)((w >> 8) & 0xFF);
            out[i + 2] = (uint8_t)((w >> 16) & 0xFF);
            out[i + 3] = (uint8_t)((w >> 24) & 0xFF);
            i += 4;
        } else if (ctx->ops->read8) {
            if (ctx->ops->read8(ctx, a, &out[i]) != 0) return -1;
            i += 1;
        } else {
            return -1;
        }
    }
    return 0;
}

int hw_flash_write(hw_t *ctx, unsigned int addr, const void *buf, size_t len) {
    if (!ctx || !ctx->ops || !ctx->ops->flash_write || !buf) return -1;
    if (len == 0) return 0;
    return ctx->ops->flash_write(ctx, addr, (const uint8_t*)buf, len);
}

int hw_flash_erase(hw_t *ctx, unsigned int addr, size_t len) {
    if (!ctx || !ctx->ops || !ctx->ops->flash_erase) return -1;
    if (len == 0) return 0;
    return ctx->ops->flash_erase(ctx, addr, len);
}

int hw_flash_mass_erase(hw_t *ctx) {
    if (!ctx || !ctx->ops || !ctx->ops->flash_mass_erase) return -1;
    return ctx->ops->flash_mass_erase(ctx);
}

int hw_flash_verify(hw_t *ctx, unsigned int addr, const void *buf, size_t len) {
    if (!ctx || !buf) return -1;
    if (len == 0) return 0;

    uint8_t *readback = malloc(len);
    if (!readback) return -1;

    int rc = -1;
    if (hw_flash_read(ctx, addr, readback, len) == 0) {
        const uint8_t *want = (const uint8_t*)buf;
        size_t i = 0;
        while (i < len && readback[i] == want[i]) i++;

        if (i == len) {
            rc = 0;
        } else {
            fprintf(stderr, "Flash verify failed at 0x%08x: read 0x%02x, expected 0x%02x\n",
                    addr + (unsigned int)i, readback[i], want[i]);
        }
    }

    free(readback);
    return rc;
}

/**
 * @brief Build the full desired image of one erase block.
 *
 * The caller's region rarely lines up with the erase blocks, so the bytes of
 * the block that lie outside it have to be read back and carried through the
 * erase. When the region covers the whole block that read is skipped.
 */
static int flash_build_sector_image(hw_t *ctx,
                                    unsigned int sect_base, unsigned int sect_size,
                                    unsigned int addr, const uint8_t *image, size_t len,
                                    uint8_t *out) {
    unsigned int sect_end = sect_base + sect_size;
    unsigned int reg_end  = addr + (unsigned int)len;

    unsigned int ov_start = (sect_base > addr)    ? sect_base : addr;
    unsigned int ov_end   = (sect_end  < reg_end) ? sect_end  : reg_end;
    if (ov_start >= ov_end) return -1;

    if (ov_start > sect_base || ov_end < sect_end) {
        if (hw_flash_read(ctx, sect_base, out, sect_size) != 0) return -1;
    }

    memcpy(out + (ov_start - sect_base), image + (ov_start - addr), ov_end - ov_start);
    return 0;
}

int hw_flash_update(hw_t *ctx, unsigned int addr, const void *buf, size_t len) {
    if (!ctx || !ctx->ops || !buf) return -1;
    if (len == 0) return 0;

    if (!ctx->ops->flash_erase || !ctx->ops->flash_write) {
        fprintf(stderr, "Backend does not support programming flash.\n");
        return -1;
    }

    hw_flash_info_t info;
    if (hw_flash_info(ctx, &info) != 0) return -1;
    if (flash_range_ok(&info, addr, len) != 0) {
        fprintf(stderr, "Flash range 0x%08x..0x%08x lies outside flash (0x%08x, %u bytes).\n",
                addr, addr + (unsigned int)len, info.base, info.size);
        return -1;
    }
    if (flash_halt_target(ctx) != 0) return -1;

    const uint8_t *image = (const uint8_t*)buf;
    unsigned int end = addr + (unsigned int)len;
    uint8_t *page = NULL;
    unsigned int page_cap = 0;
    int rc = -1;

    for (unsigned int pos = addr; pos < end; ) {
        unsigned int sb = 0, ss = 0;
        if (hw_flash_sector(ctx, pos, &sb, &ss) != 0 || ss == 0 || sb + ss <= pos) goto done;

        if (ss > page_cap) {
            uint8_t *bigger = realloc(page, ss);
            if (!bigger) goto done;
            page = bigger;
            page_cap = ss;
        }

        if (flash_build_sector_image(ctx, sb, ss, addr, image, len, page) != 0) goto done;
        if (ctx->ops->flash_erase(ctx, sb, ss) != 0) goto done;
        if (ctx->ops->flash_write(ctx, sb, page, ss) != 0) goto done;

        pos = sb + ss;
    }

    rc = hw_flash_verify(ctx, addr, buf, len);

done:
    free(page);
    return rc;
}

int hw_flash_patch(hw_t *ctx, unsigned int addr,
                   const void *old_image, const void *new_image, size_t len,
                   hw_flash_patch_stats_t *stats_out) {
    hw_flash_patch_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (stats_out) *stats_out = stats;

    if (!ctx || !ctx->ops || !new_image) return -1;
    if (len == 0) return 0;

    if (!ctx->ops->flash_erase || !ctx->ops->flash_write) {
        fprintf(stderr, "Backend does not support programming flash.\n");
        return -1;
    }

    hw_flash_info_t info;
    if (hw_flash_info(ctx, &info) != 0) return -1;
    if (flash_range_ok(&info, addr, len) != 0) {
        fprintf(stderr, "Flash range 0x%08x..0x%08x lies outside flash (0x%08x, %u bytes).\n",
                addr, addr + (unsigned int)len, info.base, info.size);
        return -1;
    }
    if (flash_halt_target(ctx) != 0) return -1;

    const uint8_t *oldi = (const uint8_t*)old_image;
    const uint8_t *newi = (const uint8_t*)new_image;
    unsigned int end = addr + (unsigned int)len;

    uint8_t *cur = NULL, *want = NULL;   // current and desired block contents
    unsigned int cap = 0;
    int rc = -1;

    for (unsigned int pos = addr; pos < end; ) {
        unsigned int sb = 0, ss = 0;
        if (hw_flash_sector(ctx, pos, &sb, &ss) != 0 || ss == 0 || sb + ss <= pos) goto done;

        unsigned int sect_end = sb + ss;
        unsigned int ov_start = (sb < addr) ? addr : sb;
        unsigned int ov_end   = (sect_end < end) ? sect_end : end;
        unsigned int ov_len   = ov_end - ov_start;

        stats.sectors_total++;

        if (ss > cap) {
            uint8_t *a = realloc(cur, ss);
            if (!a) goto done;
            cur = a;
            uint8_t *b = realloc(want, ss);
            if (!b) goto done;
            want = b;
            cap = ss;
        }

        // Step 1: decide whether this block changed at all. When the caller
        // supplied the old image this costs no target I/O, which is the whole
        // point of passing it -- untouched blocks are never even read.
        if (oldi) {
            if (memcmp(oldi + (ov_start - addr), newi + (ov_start - addr), ov_len) == 0) {
                pos = sect_end;
                continue;
            }
            if (hw_flash_read(ctx, sb, cur, ss) != 0) goto done;
        } else {
            if (hw_flash_read(ctx, sb, cur, ss) != 0) goto done;
            if (memcmp(cur + (ov_start - sb), newi + (ov_start - addr), ov_len) == 0) {
                pos = sect_end;
                continue;
            }
        }

        stats.sectors_changed++;

        // Step 2: the desired block is what is there now, with the new bytes
        // spliced over the part of it the region covers.
        memcpy(want, cur, ss);
        memcpy(want + (ov_start - sb), newi + (ov_start - addr), ov_len);

        if (memcmp(want, cur, ss) == 0) {
            // The old image disagreed with the device but the device already
            // holds what we want. Nothing to do.
            pos = sect_end;
            continue;
        }

        // Step 3: an erase is only needed if some bit has to go 0 -> 1.
        // Programming can always clear bits, so a change that only clears
        // them can be written straight over the top.
        int need_erase = 0;
        for (unsigned int i = 0; i < ss; i++) {
            if ((want[i] & cur[i]) != want[i]) { need_erase = 1; break; }
        }

        if (need_erase) {
            if (ctx->ops->flash_erase(ctx, sb, ss) != 0) goto done;
            if (ctx->ops->flash_write(ctx, sb, want, ss) != 0) goto done;
            stats.sectors_erased++;
            stats.bytes_written += ss;
        } else {
            // Reprogram only the span that actually differs, widened out to
            // the part's program granularity.
            unsigned int lo = 0, hi = ss;
            while (lo < ss && want[lo] == cur[lo]) lo++;
            while (hi > lo && want[hi - 1] == cur[hi - 1]) hi--;

            if (ss % info.write_align != 0) {
                // Odd geometry: widening could run past the end of the sector
                // or leave a ragged length, so just rewrite the whole thing.
                lo = 0;
                hi = ss;
            } else {
                lo -= lo % info.write_align;
                hi += (info.write_align - (hi % info.write_align)) % info.write_align;
                if (hi > ss) hi = ss;
            }

            if (ctx->ops->flash_write(ctx, sb + lo, want + lo, hi - lo) != 0) goto done;
            stats.bytes_written += hi - lo;
        }

        pos = sect_end;
    }

    rc = hw_flash_verify(ctx, addr, new_image, len);

done:
    free(cur);
    free(want);
    if (stats_out) *stats_out = stats;
    return rc;
}
