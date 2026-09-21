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

/* --- armv7m ------------------------------------------------------------- */

#define HW_STATE(id, name, ns, acc, enc, w, rd, wr, snap, feat) \
    { name, ns, acc, enc, w, rd, wr, snap, feat },
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

/* --- cortex_m7_r0p2 ------------------------------------------------------ */

#define HW_STATE(id, name, ns, acc, enc, w, rd, wr, snap, feat) \
    { name, ns, acc, enc, w, rd, wr, snap, feat },
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

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

const hw_state_db_t hw_state_databases[] = {
    { "armv7m", "DDI0403 E.e",
      armv7m_states, COUNT(armv7m_states),
      armv7m_aliases, COUNT(armv7m_aliases),
      armv7m_operations, COUNT(armv7m_operations) },
    { "cortex_m7_r0p2", "DDI0489 B",
      cortex_m7_r0p2_states, COUNT(cortex_m7_r0p2_states),
      cortex_m7_r0p2_aliases, COUNT(cortex_m7_r0p2_aliases),
      cortex_m7_r0p2_operations, COUNT(cortex_m7_r0p2_operations) },
    { NULL, NULL, NULL, 0, NULL, 0, NULL, 0 },
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
    if (desc->access != HW_ACC_MEMORY_MAPPED ||
        !desc->encoding || strncmp(desc->encoding, "0x", 2) != 0) {
        // Core, special and FP system registers need a backend path that does
        // not exist yet. Say so instead of reporting the state as absent.
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
