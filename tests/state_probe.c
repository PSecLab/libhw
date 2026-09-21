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

#define N_PRESENCE 6
#define N_READ     6

static void usage(const char *prog) {
    fprintf(stderr, "\nUsage: %s <backend> [--verbose]\n", prog);
    fprintf(stderr, "  Probes the generated state database against a live target.\n");
    fprintf(stderr, "  backend: stlink, openocd, mock\n\n");
}

static void probe_db(hw_t *hw, const hw_state_db_t *db,
                     const hw_cpu_features_t *f, int verbose) {
    size_t presence[N_PRESENCE] = {0};
    size_t reads[N_READ] = {0};
    size_t read_ok_presence_unknown = 0, read_zero_presence_unknown = 0;

    printf("\n--- %s (%s): %zu state elements ---\n", db->name, db->source, db->state_count);

    for (size_t i = 0; i < db->state_count; i++) {
        const hw_state_desc_t *d = &db->states[i];

        hw_state_result_t r;
        if (hw_state_query(hw, d, f, &r) != 0) continue;

        presence[r.presence]++;
        reads[r.read]++;

        // The case the report must never round up into "implemented": the read
        // worked, and nothing else says the register is there.
        if (r.read == HW_READ_OK && r.presence == HW_PRESENCE_UNKNOWN) {
            read_ok_presence_unknown++;
            if (r.value_is_zero) read_zero_presence_unknown++;
        }

        if (verbose && r.read == HW_READ_OK) {
            printf("    %-16s %-14s 0x%08X  presence=%s\n",
                   d->name, d->encoding ? d->encoding : "-", r.value,
                   hw_presence_name(r.presence));
        }
    }

    printf("  Implemented (evidence-based):\n");
    for (int k = 0; k < N_PRESENCE; k++) {
        if (presence[k]) {
            printf("      %-26s %zu\n", hw_presence_name((hw_presence_t)k), presence[k]);
        }
    }
    printf("  Read outcome (independent of the above):\n");
    for (int k = 0; k < N_READ; k++) {
        if (reads[k]) {
            printf("      %-26s %zu\n", hw_read_status_name((hw_read_status_t)k), reads[k]);
        }
    }
    if (read_ok_presence_unknown) {
        printf("  Read succeeded but presence is UNKNOWN: %zu (%zu of them read as zero)\n",
               read_ok_presence_unknown, read_zero_presence_unknown);
        printf("      These are NOT counted as implemented. An unimplemented word in the\n");
        printf("      PPB generally reads as zero rather than faulting.\n");
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
    printf("  Part                 %s r%up%u\n",
           f.cpu_name ? f.cpu_name : "not recognised", f.variant, f.revision);
    printf("  CPU overlay          %s\n",
           f.overlay ? f.overlay : "none pinned - architecture manifest only");
    if (f.overlay && !f.revision_matches) {
        printf("  NOTE                 the pinned TRM documents %s; this part is r%up%u.\n",
               f.overlay_revision, f.variant, f.revision);
        printf("                       Differences introduced after %s are not covered.\n",
               f.overlay_revision);
    }
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

    printf("\nNote: a successful read is not evidence of implementation. Presence above\n");
    printf("comes only from the architecture, from feature/configuration registers, or\n");
    printf("from CoreSight component discovery.\n");
    printf("\n--> Leaving the target halted.\n");
    hw_close(hw);
    return 0;
}
