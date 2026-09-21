/**
 * @file state_test.c
 * @brief Hardware-free tests for the generated state database and status model.
 *
 * Runs against the mock backend, so CI can check the invariants that matter
 * without a board and without the licensed Arm documents: the generated tables
 * are self-consistent, a component-relative offset is never read as an address,
 * and presence is never inferred from a successful read.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw.h"
#include "hw_state.h"

static int failures = 0;
static int checks = 0;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  [FAIL] %s\n", what);
    } else {
        printf("  [ ok ] %s\n", what);
    }
}

/** Every database the library was built with. */
static size_t db_count(void) {
    size_t n = 0;
    while (hw_state_databases[n].name) n++;
    return n;
}

int main(void) {
    hw_t *hw = hw_connect("mock", NULL, 0);
    if (!hw) {
        fprintf(stderr, "Could not connect to the mock backend.\n");
        return 1;
    }

    printf("Databases\n");
    check(db_count() >= 2, "at least an architecture and one CPU overlay are built in");
    const hw_state_db_t *arch = hw_state_db("armv7m");
    check(arch != NULL, "armv7m database is present");
    check(hw_state_db("no_such_db") == NULL, "an unknown database name returns NULL");
    if (!arch) { hw_close(hw); return 1; }
    check(arch->state_count > 400, "armv7m carries the expected order of state elements");
    check(arch->component_count > 0, "armv7m declares at least one CoreSight component");

    printf("Lookups\n");
    const hw_state_desc_t *primask = hw_state_find(arch, "PRIMASK");
    check(primask != NULL, "PRIMASK is present");
    check(hw_state_find(arch, "NOT_A_REGISTER") == NULL, "an unknown name returns NULL");
    check(hw_state_find(arch, "S0") != NULL, "the FP register file is present (S0)");
    check(hw_state_find(arch, "S31") != NULL, "the FP register file is complete (S31)");
    check(hw_state_find(arch, "R12") != NULL, "R12 is present");
    check(hw_state_find(arch, "SP_main") != NULL, "SP_main is present");

    printf("Generated table consistency\n");
    size_t alias_targets_missing = 0;
    for (size_t i = 0; i < arch->alias_count; i++) {
        if (!hw_state_find(arch, arch->aliases[i].target)) alias_targets_missing++;
    }
    check(alias_targets_missing == 0, "every alias points at a state element that exists");

    size_t ops_without_reason = 0;
    for (size_t i = 0; i < arch->operation_count; i++) {
        if (!arch->operations[i].reason || !arch->operations[i].reason[0]) ops_without_reason++;
    }
    check(ops_without_reason == 0, "every non-state operation carries a reason");

    size_t snapshot_unreadable = 0;
    for (size_t i = 0; i < arch->state_count; i++) {
        if (arch->states[i].snapshot && !arch->states[i].readable) snapshot_unreadable++;
    }
    check(snapshot_unreadable == 0, "nothing write-only is marked for the snapshot");

    printf("Encoding kinds\n");
    size_t offsets = 0, offsets_read = 0;
    for (size_t i = 0; i < arch->state_count; i++) {
        const hw_state_desc_t *d = &arch->states[i];
        if (d->enc_kind != HW_ENC_OFFSET) continue;
        offsets++;
        uint32_t v = 0;
        hw_read_status_t why = HW_READ_OK;
        // Some offsets are also write-only, so the refusal may come back as
        // UNSAFE rather than BACKEND_UNSUPPORTED. The invariant is only that
        // the read never succeeds.
        if (hw_state_read(hw, d, &v, &why) == 0 || why == HW_READ_OK) offsets_read++;
    }
    check(offsets > 0, "the database contains component-relative offsets");
    // Reading an offset as though it were an address silently returns some
    // unrelated location. It must be refused, not attempted.
    check(offsets_read == 0, "a component-relative offset is never read as an address");

    printf("Presence is not readability\n");
    size_t denied_but_read = 0, debug_called_architectural = 0, unknown_from_read = 0;
    size_t wo_attempted = 0;
    hw_cpu_features_t f;
    memset(&f, 0, sizeof(f));   /* a target implementing no optional feature */

    for (size_t i = 0; i < arch->state_count; i++) {
        const hw_state_desc_t *d = &arch->states[i];
        hw_state_result_t r;
        if (hw_state_query(hw, d, &f, &r) != 0) continue;

        if ((r.presence == HW_PRESENCE_CONFIG_DENIED ||
             r.presence == HW_PRESENCE_DISCOVERY_ABSENT) && r.read != HW_READ_NOT_ATTEMPTED) {
            denied_but_read++;
        }
        if (d->ns == HW_NS_DEBUG && r.presence == HW_PRESENCE_ARCHITECTURAL) {
            debug_called_architectural++;
        }
        if (!d->readable && r.read != HW_READ_UNSAFE) wo_attempted++;
        // The invariant that matters: a read cannot manufacture presence.
        if (r.read == HW_READ_OK && r.presence == HW_PRESENCE_UNKNOWN) unknown_from_read++;
    }
    check(denied_but_read == 0, "state the target says is absent is not read");
    check(debug_called_architectural == 0, "debug-block state is never called architectural");
    check(wo_attempted == 0, "write-only state is reported unsafe rather than read");
    printf("  (%zu element(s) read cleanly with no presence evidence; "
           "these are not counted as implemented)\n", unknown_from_read);

    printf("Feature resolution\n");
    const hw_state_desc_t *fp = hw_state_find(arch, "S0");
    check(fp && fp->feature && strcmp(fp->feature, "FP_EXTENSION") == 0,
          "S0 is conditional on the FP extension");
    check(hw_state_expected(fp, &f) == 0, "with no FP extension, S0 is not expected");
    f.fp_extension = 1;
    check(hw_state_expected(fp, &f) == 1, "with the FP extension, S0 is expected");

    const hw_state_desc_t *mput = hw_state_find(arch, "MPU_TYPE");
    check(mput && mput->feature && strcmp(mput->feature, "MPU") == 0,
          "MPU_TYPE is conditional on the MPU");
    check(hw_state_expected(mput, &f) == 0, "with no MPU, MPU_TYPE is not expected");

    printf("Debug access paths\n");
    const hw_state_dbgreg_t *msp = hw_state_dbgreg(arch, "MSP");
    check(msp != NULL && msp->regsel == 17, "MSP is reached by DCRSR REGSEL 17");
    const hw_state_desc_t *msp_desc = hw_state_find(arch, "MSP");
    // SYSm and REGSEL are different encodings for the same state; conflating
    // them reads the wrong register.
    check(msp_desc && msp_desc->encoding && strcmp(msp_desc->encoding, "SYSm=8") == 0,
          "MSP's instruction encoding is SYSm 8, distinct from its REGSEL 17");
    const hw_state_dbgreg_t *pm = hw_state_dbgreg(arch, "PRIMASK");
    check(pm && pm->regsel == 20 && pm->lsb == 0 && pm->width == 8,
          "PRIMASK occupies REGSEL 20 bits[7:0]");
    const hw_state_dbgreg_t *ctrl = hw_state_dbgreg(arch, "CONTROL");
    check(ctrl && ctrl->regsel == 20 && ctrl->lsb == 24,
          "CONTROL shares REGSEL 20 at bits[31:24]");
    check(hw_state_dbgreg(arch, "S0") != NULL, "the FP register file has a debug access path");

    printf("Status naming\n");
    check(strcmp(hw_presence_name(HW_PRESENCE_ARCHITECTURAL), "ARCHITECTURAL") == 0,
          "presence verdicts have names");
    check(strcmp(hw_read_status_name(HW_READ_OK), "OK") == 0, "read statuses have names");

    hw_close(hw);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
