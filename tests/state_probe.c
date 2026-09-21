/**
 * @file state_probe.c
 * @brief Validate the generated state database against a live target.
 *
 * Reads the target's ID registers, derives which optional state it implements,
 * then walks the expected state set and attempts a safe read of each element.
 *
 * The point of the exercise is the classification, not the values: a register
 * that cannot be read here has not been shown to be absent from the
 * architecture. Runtime inaccessibility and architectural absence are
 * different things and are reported separately.
 *
 * This only ever reads. The one write it performs is to DCRSR, which is how a
 * debugger selects a core register for transfer, and it is done only while the
 * target is halted.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw.h"
#include "hw_state.h"

#define AVAIL_KINDS 6

static void usage(const char *prog) {
    fprintf(stderr, "\nUsage: %s <backend> [--verbose]\n", prog);
    fprintf(stderr, "  Probes the generated state database against a live target.\n");
    fprintf(stderr, "  backend: stlink, openocd, mock\n\n");
}

static void probe_db(hw_t *hw, const hw_state_db_t *db,
                     const hw_cpu_features_t *f, int verbose) {
    size_t counts[AVAIL_KINDS] = {0};
    size_t not_expected = 0, unresolved = 0, skipped_wo = 0;

    printf("\n--- %s (%s): %zu state elements ---\n", db->name, db->source, db->state_count);

    for (size_t i = 0; i < db->state_count; i++) {
        const hw_state_desc_t *d = &db->states[i];

        int expect = hw_state_expected(d, f);
        if (expect == 0) {
            // The target tells us this optional state is not implemented. That
            // is a property of this CPU, not of the architecture.
            not_expected++;
            continue;
        }
        if (expect < 0) {
            unresolved++;
            continue;
        }
        if (!d->readable) {
            skipped_wo++;
            continue;
        }

        uint32_t value = 0;
        hw_state_avail_t why = HW_STATE_READ_FAILED;
        int rc = hw_state_read(hw, d, &value, &why);
        counts[why]++;

        if (verbose && rc == 0) {
            printf("    %-16s %-14s 0x%08X\n", d->name, d->encoding, value);
        }
    }

    printf("  %-26s %zu\n", "not implemented by CPU:", not_expected);
    printf("  %-26s %zu\n", "write-only (not sampled):", skipped_wo);
    if (unresolved) {
        printf("  %-26s %zu\n", "UNRESOLVED feature rule:", unresolved);
    }
    for (int k = 0; k < AVAIL_KINDS; k++) {
        if (counts[k]) {
            printf("  %-26s %zu\n", hw_state_avail_name((hw_state_avail_t)k), counts[k]);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    const char *backend = argv[1];
    int verbose = (argc > 2 && strcmp(argv[2], "--verbose") == 0);

    hw_t *hw = hw_connect(backend, NULL, 0);
    if (!hw) return 1;

    if (!hw_board_halted(hw)) {
        printf("--> Halting the target (core register transfers need Debug state).\n");
        hw_board_halt(hw);
    }

    hw_cpu_features_t f;
    if (hw_state_identify(hw, &f) != 0) {
        fprintf(stderr, "Failed to read the target's ID registers.\n");
        hw_close(hw);
        return 1;
    }

    printf("\n=== Target identification ===\n");
    printf("  CPUID                0x%08X  (PARTNO 0x%03X)\n", f.cpuid, f.partno);
    printf("  Part                 %s\n", f.cpu_name ? f.cpu_name : "not recognised");
    printf("  CPU overlay          %s\n",
           f.overlay ? f.overlay : "none pinned - architecture manifest only");
    printf("  FP extension         %s\n", f.fp_extension ? "implemented" : "not implemented");
    printf("  MPU regions          %u\n", f.mpu_regions);
    printf("  DWT comparators      %u\n", f.dwt_numcomp);
    printf("  NVIC INTLINESNUM     %u  (%u interrupt lines)\n",
           f.nvic_intlinesnum, (f.nvic_intlinesnum + 1) * 32);

    const hw_state_db_t *arch = hw_state_db("armv7m");
    if (arch) probe_db(hw, arch, &f, verbose);

    if (f.overlay) {
        const hw_state_db_t *cpu = hw_state_db(f.overlay);
        if (cpu) probe_db(hw, cpu, &f, verbose);
    } else {
        printf("\n--- CPU overlay ---\n");
        printf("  No TRM is pinned for PARTNO 0x%03X, so no implementation-defined\n", f.partno);
        printf("  state is claimed for this part. That is a gap in our sources,\n");
        printf("  not a statement that the part adds no state.\n");
    }

    printf("\n--> Leaving the target halted.\n");
    hw_close(hw);
    return 0;
}
