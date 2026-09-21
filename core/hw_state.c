/**
 * @file hw_state.c
 * @brief Builds the generated state descriptor tables and reads them from a target.
 *
 * The tables come straight from the generated .def files; this file only gives
 * them storage and a small amount of behaviour.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw_state.h"

static int hw_state_read_dbgreg(hw_t *ctx, const hw_state_dbgreg_t *reg,
                                uint32_t *value_out, hw_state_avail_t *why);

/* --- armv7m ------------------------------------------------------------- */

#define HW_STATE(id, name, ns, acc, enc, ekind, w, rd, wr, snap, feat) \
    { name, ns, acc, enc, ekind, w, rd, wr, snap, feat },
static const hw_state_desc_t armv7m_states[] = {
#include "armv7m.def"
};

#define HW_ALIAS(id, target, off, width) { #id, #target, off, width },
static const hw_state_alias_t armv7m_aliases[] = {
#include "armv7m.def"
};

#define HW_OPERATION(id, name, enc, reason) { name, enc, reason },
static const hw_state_operation_t armv7m_operations[] = {
#include "armv7m.def"
};

#define HW_DBGREG(id, name, regsel, lsb, width) { name, regsel, lsb, width },
static const hw_state_dbgreg_t armv7m_dbgregs[] = {
#include "armv7m.def"
};

/* --- cortex_m7_r0p2 ------------------------------------------------------ */

#define HW_STATE(id, name, ns, acc, enc, ekind, w, rd, wr, snap, feat) \
    { name, ns, acc, enc, ekind, w, rd, wr, snap, feat },
static const hw_state_desc_t cortex_m7_r0p2_states[] = {
#include "cortex_m7_r0p2.def"
};

#define HW_ALIAS(id, target, off, width) { #id, #target, off, width },
static const hw_state_alias_t cortex_m7_r0p2_aliases[] = {
#include "cortex_m7_r0p2.def"
};

#define HW_OPERATION(id, name, enc, reason) { name, enc, reason },
static const hw_state_operation_t cortex_m7_r0p2_operations[] = {
#include "cortex_m7_r0p2.def"
};

#define HW_DBGREG(id, name, regsel, lsb, width) { name, regsel, lsb, width },
static const hw_state_dbgreg_t cortex_m7_r0p2_dbgregs[] = {
#include "cortex_m7_r0p2.def"
};

/* --- cortex_m4_r0p0 ------------------------------------------------------ */

#define HW_STATE(id, name, ns, acc, enc, ekind, w, rd, wr, snap, feat) \
    { name, ns, acc, enc, ekind, w, rd, wr, snap, feat },
static const hw_state_desc_t cortex_m4_r0p0_states[] = {
#include "cortex_m4_r0p0.def"
};

#define HW_ALIAS(id, target, off, width) { #id, #target, off, width },
static const hw_state_alias_t cortex_m4_r0p0_aliases[] = {
#include "cortex_m4_r0p0.def"
};

#define HW_OPERATION(id, name, enc, reason) { name, enc, reason },
static const hw_state_operation_t cortex_m4_r0p0_operations[] = {
#include "cortex_m4_r0p0.def"
};

#define HW_DBGREG(id, name, regsel, lsb, width) { name, regsel, lsb, width },
static const hw_state_dbgreg_t cortex_m4_r0p0_dbgregs[] = {
#include "cortex_m4_r0p0.def"
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

const hw_state_db_t hw_state_databases[] = {
    { "armv7m", "DDI0403 E.e",
      armv7m_states, COUNT(armv7m_states),
      armv7m_aliases, COUNT(armv7m_aliases),
      armv7m_operations, COUNT(armv7m_operations),
      armv7m_dbgregs, COUNT(armv7m_dbgregs) },
    { "cortex_m7_r0p2", "DDI0489 B",
      cortex_m7_r0p2_states, COUNT(cortex_m7_r0p2_states),
      cortex_m7_r0p2_aliases, COUNT(cortex_m7_r0p2_aliases),
      cortex_m7_r0p2_operations, COUNT(cortex_m7_r0p2_operations),
      cortex_m7_r0p2_dbgregs, COUNT(cortex_m7_r0p2_dbgregs) },
    { "cortex_m4_r0p0", "DDI0439 B",
      cortex_m4_r0p0_states, COUNT(cortex_m4_r0p0_states),
      cortex_m4_r0p0_aliases, COUNT(cortex_m4_r0p0_aliases),
      cortex_m4_r0p0_operations, COUNT(cortex_m4_r0p0_operations),
      cortex_m4_r0p0_dbgregs, COUNT(cortex_m4_r0p0_dbgregs) },
    { NULL, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0 },
};

const hw_state_db_t *hw_state_db(const char *name) {
    if (!name) return NULL;
    for (int i = 0; hw_state_databases[i].name; i++) {
        if (strcmp(hw_state_databases[i].name, name) == 0) {
            return &hw_state_databases[i];
        }
    }
    return NULL;
}

const hw_state_desc_t *hw_state_find(const hw_state_db_t *db, const char *name) {
    if (!db || !name) return NULL;
    for (size_t i = 0; i < db->state_count; i++) {
        if (strcmp(db->states[i].name, name) == 0) {
            return &db->states[i];
        }
    }
    return NULL;
}

int hw_state_read(hw_t *ctx, const hw_state_desc_t *desc,
                  uint32_t *value_out, hw_state_avail_t *why) {
    hw_state_avail_t ignored;
    if (!why) why = &ignored;

    if (!ctx || !desc || !value_out) {
        *why = HW_STATE_READ_FAILED;
        return -1;
    }
    if (!desc->readable) {
        // Write-only registers have no value to sample; reading some of them
        // has side effects, so refuse rather than try.
        *why = HW_STATE_UNSAFE_TO_READ;
        return -1;
    }
    if (desc->enc_kind == HW_ENC_OFFSET) {
        // The manual gives this register as an offset within a CoreSight
        // component and we do not record component base addresses yet. Reading
        // the offset as though it were an address would quietly return some
        // unrelated location's contents and report success.
        *why = HW_STATE_BACKEND_UNSUPPORTED;
        return -1;
    }

    if (desc->access != HW_ACC_MEMORY_MAPPED || desc->enc_kind != HW_ENC_ABSOLUTE ||
        !desc->encoding || strncmp(desc->encoding, "0x", 2) != 0) {
        // Core, special-purpose and FP registers are reached through
        // DCRSR/DCRDR rather than by a plain load.
        const hw_state_dbgreg_t *reg = NULL;
        for (int i = 0; hw_state_databases[i].name && !reg; i++) {
            reg = hw_state_dbgreg(&hw_state_databases[i], desc->name);
        }
        if (reg) {
            return hw_state_read_dbgreg(ctx, reg, value_out, why);
        }
        *why = HW_STATE_BACKEND_UNSUPPORTED;
        return -1;
    }

    unsigned int addr = (unsigned int)strtoul(desc->encoding, NULL, 16);
    unsigned int v = 0;
    if (hw_read32(ctx, addr, &v) != 0) {
        *why = HW_STATE_READ_FAILED;
        return -1;
    }

    *value_out = (uint32_t)v;
    *why = HW_STATE_AVAILABLE;
    return 0;
}

/* --- Identification and feature resolution ------------------------------ */

/*
 * Addresses used for identification. Each is an entry in the generated
 * armv7m database; they are named here because the resolver has to read them
 * before it can say which other state to expect.
 */
#define SCS_CPUID     0xE000ED00u   /* DDI0403 E.e, Table B3-4  */
#define SCS_ICTR      0xE000E004u   /* DDI0403 E.e, Table B3-6  */
#define SCS_MPU_TYPE  0xE000ED90u   /* DDI0403 E.e, Table B3-11 */
#define SCS_MVFR0     0xE000EF40u   /* DDI0403 E.e, Table B3-5  */
#define DWT_CTRL      0xE0001000u   /* DDI0403 E.e, Table C1-21 */

#define DCRSR_ADDR    0xE000EDF4u   /* DDI0403 E.e, C1.6.3 */
#define DCRDR_ADDR    0xE000EDF8u   /* DDI0403 E.e, C1.6.4 */
#define DHCSR_ADDR    0xE000EDF0u   /* DDI0403 E.e, C1.6.2 */
#define DHCSR_S_REGRDY (1u << 16)

/* CPUID PARTNO of the one Cortex-M part we hold a TRM for: DDI0489B Table 3-1
 * gives a CPUID reset value of 0x410FC272, so PARTNO is 0xC27. */
#define PARTNO_CORTEX_M7 0xC27u
/* DDI0439B Table 4-1 gives a CPUID reset value of 0x410FC240, so PARTNO 0xC24. */
#define PARTNO_CORTEX_M4 0xC24u

const char *hw_state_avail_name(hw_state_avail_t a) {
    switch (a) {
    case HW_STATE_AVAILABLE:              return "AVAILABLE";
    case HW_STATE_NOT_IMPLEMENTED_BY_CPU: return "NOT_IMPLEMENTED_BY_CPU";
    case HW_STATE_BACKEND_UNSUPPORTED:    return "BACKEND_UNSUPPORTED";
    case HW_STATE_ACCESS_DENIED:          return "ACCESS_DENIED";
    case HW_STATE_UNSAFE_TO_READ:         return "UNSAFE_TO_READ";
    case HW_STATE_READ_FAILED:            return "READ_FAILED";
    }
    return "?";
}

int hw_state_identify(hw_t *ctx, hw_cpu_features_t *out) {
    if (!ctx || !out) return -1;
    memset(out, 0, sizeof(*out));

    unsigned int v = 0;
    if (hw_read32(ctx, SCS_CPUID, &v) != 0) return -1;
    out->cpuid = v;
    out->partno = (uint16_t)((v >> 4) & 0xFFFu);

    out->variant = (uint8_t)((v >> 20) & 0xFu);
    out->revision = (uint8_t)(v & 0xFu);

    if (out->partno == PARTNO_CORTEX_M7) {
        out->cpu_name = "Cortex-M7";
        out->overlay = "cortex_m7_r0p2";
        out->overlay_revision = "r0p2";
        out->revision_matches = (out->variant == 0 && out->revision == 2);
    } else if (out->partno == PARTNO_CORTEX_M4) {
        out->cpu_name = "Cortex-M4";
        out->overlay = "cortex_m4_r0p0";
        out->overlay_revision = "r0p0";
        // The pinned TRM documents r0p0. A part at a later revision is still
        // covered for everything the TRM describes, but differences introduced
        // after r0p0 are not, so the caller is told rather than left to assume.
        out->revision_matches = (out->variant == 0 && out->revision == 0);
    } else {
        // Not a fault: we simply hold no TRM for this part, so only the
        // architecture manifest applies. Saying so beats guessing an overlay.
        out->cpu_name = NULL;
        out->overlay = NULL;
        out->overlay_revision = NULL;
        out->revision_matches = 0;
    }

    if (hw_read32(ctx, SCS_MPU_TYPE, &v) == 0) {
        out->mpu_regions = (uint8_t)((v >> 8) & 0xFFu);
        out->mpu = out->mpu_regions != 0;
    }
    if (hw_read32(ctx, SCS_MVFR0, &v) == 0) {
        // A part without the FP extension reads this reserved SCS word as zero.
        out->fp_extension = (v != 0);
    }
    if (hw_read32(ctx, DWT_CTRL, &v) == 0) {
        out->dwt_numcomp = (uint8_t)((v >> 28) & 0xFu);
    }
    if (hw_read32(ctx, SCS_ICTR, &v) == 0) {
        out->nvic_intlinesnum = (uint8_t)(v & 0xFu);
    }
    return 0;
}

int hw_state_expected(const hw_state_desc_t *desc, const hw_cpu_features_t *f) {
    if (!desc) return 0;
    if (!desc->feature || !desc->feature[0]) return 1;
    if (!f) return 1;

    if (strcmp(desc->feature, "FP_EXTENSION") == 0) return f->fp_extension;
    if (strcmp(desc->feature, "MPU") == 0) return f->mpu;
    if (strcmp(desc->feature, "IMPDEF:DWT_CTRL.NUMCOMP") == 0) return f->dwt_numcomp > 0;

    // An unknown requirement must not silently mean "expected"; treat it as
    // unresolved so the probe reports it rather than hiding it.
    return -1;
}

/**
 * @brief Read a core, special-purpose or FP register through DCRSR/DCRDR.
 *
 * Writing DCRSR is a write to the target, so this requires Debug state. The
 * processor clears DHCSR.S_REGRDY on the write and sets it again when the
 * transfer completes.
 */
static int hw_state_read_dbgreg(hw_t *ctx, const hw_state_dbgreg_t *reg,
                                uint32_t *value_out, hw_state_avail_t *why) {
    if (!hw_board_halted(ctx)) {
        *why = HW_STATE_UNSAFE_TO_READ;   // core register transfers need Debug state
        return -1;
    }
    if (hw_write32(ctx, DCRSR_ADDR, reg->regsel) != 0) {
        *why = HW_STATE_READ_FAILED;
        return -1;
    }

    unsigned int dhcsr = 0;
    for (int spin = 0; spin < 100; spin++) {
        if (hw_read32(ctx, DHCSR_ADDR, &dhcsr) != 0) {
            *why = HW_STATE_READ_FAILED;
            return -1;
        }
        if (dhcsr & DHCSR_S_REGRDY) break;
    }
    if (!(dhcsr & DHCSR_S_REGRDY)) {
        *why = HW_STATE_READ_FAILED;
        return -1;
    }

    unsigned int v = 0;
    if (hw_read32(ctx, DCRDR_ADDR, &v) != 0) {
        *why = HW_STATE_READ_FAILED;
        return -1;
    }

    uint32_t mask = (reg->width >= 32) ? 0xFFFFFFFFu : ((1u << reg->width) - 1u);
    *value_out = ((uint32_t)v >> reg->lsb) & mask;
    *why = HW_STATE_AVAILABLE;
    return 0;
}

const hw_state_dbgreg_t *hw_state_dbgreg(const hw_state_db_t *db, const char *name) {
    if (!db || !name) return NULL;
    for (size_t i = 0; i < db->dbgreg_count; i++) {
        if (strcmp(db->dbgregs[i].name, name) == 0) return &db->dbgregs[i];
    }
    return NULL;
}
