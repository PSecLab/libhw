/**
 * @file flash_test.c
 * @brief End-to-end tests for the libhw flash API, run against the mock backend.
 *
 * The mock models NOR semantics faithfully -- erase sets 0xFF, programming can
 * only clear bits, and writes must respect the program granularity -- so these
 * tests exercise the real decision making in hw_flash_update() and
 * hw_flash_patch() rather than a simplified stand-in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw.h"

#define FLASH_BASE 0x08000000u
#define PAGE       1024u
#define REGION     (4u * PAGE)

static int failures = 0;
static int checks   = 0;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        printf("  [FAIL] %s\n", what);
    } else {
        printf("  [ ok ] %s\n", what);
    }
}

/** A deterministic, non-trivial byte pattern. */
static void fill_pattern(uint8_t *buf, size_t len, unsigned int seed) {
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }
}

/** Read back [addr, addr+len) and compare against 'want'. */
static int flash_matches(hw_t *hw, unsigned int addr, const uint8_t *want, size_t len) {
    uint8_t *got = malloc(len);
    if (!got) return 0;
    int ok = (hw_flash_read(hw, addr, got, len) == 0) && (memcmp(got, want, len) == 0);
    free(got);
    return ok;
}

int main(void) {
    hw_t *hw = hw_connect("mock", NULL, 0);
    if (!hw) {
        fprintf(stderr, "Could not connect to the mock backend.\n");
        return 1;
    }

    // --- Geometry -------------------------------------------------------
    printf("Geometry\n");
    hw_flash_info_t info;
    check(hw_flash_info(hw, &info) == 0, "hw_flash_info succeeds");
    check(info.base == FLASH_BASE, "flash base is 0x08000000");
    check(info.page_size == PAGE, "erase granularity is 1 KB");
    check(info.write_align == 4, "program granularity is 4 bytes");

    // The mock deliberately leaves flash_sector NULL, so this exercises the
    // core's uniform-layout fallback.
    unsigned int sb = 0, ss = 0;
    check(hw_flash_sector(hw, FLASH_BASE + PAGE + 500, &sb, &ss) == 0, "hw_flash_sector succeeds");
    check(sb == FLASH_BASE + PAGE && ss == PAGE, "sector lookup rounds down to the page");

    // --- Erase ----------------------------------------------------------
    printf("Erase\n");
    check(hw_flash_mass_erase(hw) == 0, "hw_flash_mass_erase succeeds");

    uint8_t *erased = malloc(REGION);
    memset(erased, 0xFF, REGION);
    check(flash_matches(hw, FLASH_BASE, erased, REGION), "erased flash reads back as 0xFF");

    // --- Raw write does not erase ---------------------------------------
    printf("Raw write\n");
    uint8_t raw[4] = { 0x0F, 0x0F, 0x0F, 0x0F };
    check(hw_flash_write(hw, FLASH_BASE, raw, sizeof(raw)) == 0, "hw_flash_write succeeds");
    check(flash_matches(hw, FLASH_BASE, raw, sizeof(raw)), "written bytes read back");

    uint8_t raise[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    check(hw_flash_write(hw, FLASH_BASE, raise, sizeof(raise)) == 0, "write that raises bits is accepted");
    check(flash_matches(hw, FLASH_BASE, raw, sizeof(raw)), "...but cannot actually set bits back to 1");

    // --- Update ---------------------------------------------------------
    printf("Update\n");
    uint8_t *base_image = malloc(REGION);
    fill_pattern(base_image, REGION, 1);
    check(hw_flash_update(hw, FLASH_BASE, base_image, REGION) == 0, "hw_flash_update over whole pages");
    check(flash_matches(hw, FLASH_BASE, base_image, REGION), "region reads back as written");

    // An update whose ends fall inside pages must carry the surrounding bytes
    // through the erase untouched.
    unsigned int inner_addr = FLASH_BASE + PAGE + 300;
    size_t inner_len = 500;
    uint8_t inner[500];
    fill_pattern(inner, inner_len, 99);
    check(hw_flash_update(hw, inner_addr, inner, inner_len) == 0, "hw_flash_update on an unaligned range");
    check(flash_matches(hw, inner_addr, inner, inner_len), "unaligned region reads back as written");

    memcpy(base_image + (inner_addr - FLASH_BASE), inner, inner_len);
    check(flash_matches(hw, FLASH_BASE, base_image, REGION), "bytes outside the range survived the erase");

    // --- Patch: no-op ---------------------------------------------------
    printf("Patch: identical images\n");
    hw_flash_patch_stats_t stats;
    check(hw_flash_patch(hw, FLASH_BASE, base_image, base_image, REGION, &stats) == 0,
          "patching to an identical image succeeds");
    check(stats.sectors_total == 4, "all four sectors were considered");
    check(stats.sectors_changed == 0, "no sector was touched");
    check(stats.bytes_written == 0, "nothing was written");

    // --- Patch: change only clears bits ---------------------------------
    printf("Patch: change that only clears bits\n");
    uint8_t *new_image = malloc(REGION);
    memcpy(new_image, base_image, REGION);

    unsigned int spot = 2 * PAGE + 64;          // inside sector 2
    memset(base_image + spot, 0xFF, 4);
    check(hw_flash_update(hw, FLASH_BASE + spot, base_image + spot, 4) == 0, "seed 0xFF at the edit site");
    memcpy(new_image, base_image, REGION);
    memset(new_image + spot, 0xF0, 4);          // 0xFF -> 0xF0 only clears bits

    check(hw_flash_patch(hw, FLASH_BASE, base_image, new_image, REGION, &stats) == 0,
          "patch succeeds");
    check(stats.sectors_changed == 1, "exactly one sector changed");
    check(stats.sectors_erased == 0, "no erase was needed");
    check(stats.bytes_written < PAGE, "only part of the sector was reprogrammed");
    check(flash_matches(hw, FLASH_BASE, new_image, REGION), "flash matches the new image");

    memcpy(base_image, new_image, REGION);

    // --- Patch: change needs a bit set ----------------------------------
    printf("Patch: change that needs a bit set\n");
    unsigned int spot2 = 3 * PAGE + 10;         // inside sector 3
    memset(base_image + spot2, 0x00, 4);
    check(hw_flash_update(hw, FLASH_BASE + spot2, base_image + spot2, 4) == 0, "seed 0x00 at the edit site");
    memcpy(new_image, base_image, REGION);
    memset(new_image + spot2, 0x01, 4);         // 0x00 -> 0x01 needs a bit raised

    check(hw_flash_patch(hw, FLASH_BASE, base_image, new_image, REGION, &stats) == 0,
          "patch succeeds");
    check(stats.sectors_changed == 1, "exactly one sector changed");
    check(stats.sectors_erased == 1, "that sector had to be erased");
    check(stats.bytes_written == PAGE, "the whole sector was rewritten");
    check(flash_matches(hw, FLASH_BASE, new_image, REGION), "flash matches the new image");

    memcpy(base_image, new_image, REGION);

    // --- Patch without an old image -------------------------------------
    printf("Patch: old image read back from the target\n");
    memcpy(new_image, base_image, REGION);
    fill_pattern(new_image + PAGE, PAGE, 7);    // rewrite sector 1 entirely

    check(hw_flash_patch(hw, FLASH_BASE, NULL, new_image, REGION, &stats) == 0,
          "patch with a NULL old image succeeds");
    check(stats.sectors_changed == 1, "only the sector that differs was touched");
    check(flash_matches(hw, FLASH_BASE, new_image, REGION), "flash matches the new image");

    // --- Patch across a partial range ------------------------------------
    printf("Patch: unaligned range\n");
    memcpy(base_image, new_image, REGION);
    uint8_t edit[300];
    fill_pattern(edit, sizeof(edit), 4242);
    unsigned int edit_addr = FLASH_BASE + PAGE + PAGE - 100;   // straddles sectors 1 and 2

    uint8_t old_slice[300];
    memcpy(old_slice, base_image + (edit_addr - FLASH_BASE), sizeof(edit));
    check(hw_flash_patch(hw, edit_addr, old_slice, edit, sizeof(edit), &stats) == 0,
          "patch across a sector boundary succeeds");
    check(stats.sectors_total == 2, "the range spans two sectors");
    check(flash_matches(hw, edit_addr, edit, sizeof(edit)), "the edited range reads back");

    memcpy(base_image + (edit_addr - FLASH_BASE), edit, sizeof(edit));
    check(flash_matches(hw, FLASH_BASE, base_image, REGION), "bytes outside the range are unchanged");

    // --- Bounds ----------------------------------------------------------
    printf("Bounds\n");
    uint8_t byte = 0;
    check(hw_flash_update(hw, FLASH_BASE + info.size, &byte, 1) != 0, "update past the end is rejected");
    check(hw_flash_patch(hw, FLASH_BASE - 4, NULL, &byte, 1, NULL) != 0, "patch below the base is rejected");
    check(hw_flash_update(hw, FLASH_BASE, &byte, 0) == 0, "a zero-length update is a no-op");

    free(erased);
    free(base_image);
    free(new_image);
    hw_close(hw);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
