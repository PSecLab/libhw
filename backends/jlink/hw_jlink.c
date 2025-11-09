#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

// --- Define minimal function pointer types ---
typedef int (*JLINKARM_OPEN)(void);
typedef void (*JLINKARM_CLOSE)(void);
typedef int (*JLINKARM_CONNECT)(void);
typedef unsigned int (*JLINKARM_GETSN)(void);
typedef void (*JLINKARM_RESET)(void);
typedef void (*JLINKARM_GO)(void);
typedef void (*JLINKARM_HALT)(void);
typedef int (*JLINKARM_READMEMU32)(uint32_t addr, uint32_t count, uint32_t *buffer, uint8_t *status);

// --- Helper to get function safely ---
#define RESOLVE(sym) ({                                             \
    void *ptr = dlsym(handle, #sym);                                \
    if (!ptr) { fprintf(stderr, "Missing symbol: %s\n", #sym); exit(1); } \
    (sym##_t)ptr; })

int main(void) {
    // Load the J-Link shared library
    const char *lib_path = "/opt/SEGGER/JLink/libjlinkarm.so";
    void *handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Failed to load %s: %s\n", lib_path, dlerror());
        return 1;
    }

    // Load symbols
    JLINKARM_OPEN JLINKARM_Open = (JLINKARM_OPEN)dlsym(handle, "JLINKARM_Open");
    JLINKARM_CLOSE JLINKARM_Close = (JLINKARM_CLOSE)dlsym(handle, "JLINKARM_Close");
    JLINKARM_CONNECT JLINKARM_Connect = (JLINKARM_CONNECT)dlsym(handle, "JLINKARM_Connect");
    JLINKARM_GETSN JLINKARM_GetSN = (JLINKARM_GETSN)dlsym(handle, "JLINKARM_GetSN");
    JLINKARM_RESET JLINKARM_Reset = (JLINKARM_RESET)dlsym(handle, "JLINKARM_Reset");
    JLINKARM_GO JLINKARM_Go = (JLINKARM_GO)dlsym(handle, "JLINKARM_Go");
    JLINKARM_HALT JLINKARM_Halt = (JLINKARM_HALT)dlsym(handle, "JLINKARM_Halt");
    JLINKARM_READMEMU32 JLINKARM_ReadMemU32 = (JLINKARM_READMEMU32)dlsym(handle, "JLINKARM_ReadMemU32");

    if (!JLINKARM_Open || !JLINKARM_Close || !JLINKARM_Connect || !JLINKARM_GetSN) {
        fprintf(stderr, "Missing one or more required symbols.\n");
        dlclose(handle);
        return 1;
    }

    // Open J-Link
    if (JLINKARM_Open() < 0) {
        fprintf(stderr, "Could not open J-Link.\n");
        dlclose(handle);
        return 1;
    }

    unsigned int sn = JLINKARM_GetSN();
    printf("Connected to J-Link, SN: %u\n", sn);

    // Connect to target (Cortex-M assumed)
    if (JLINKARM_Connect() < 0) {
        fprintf(stderr, "Failed to connect to target.\n");
        JLINKARM_Close();
        dlclose(handle);
        return 1;
    }

    printf("Target connected.\n");

    // Halt CPU and read memory
    if (JLINKARM_Halt) {
        JLINKARM_Halt();
    }

    uint32_t val = 0;
    if (JLINKARM_ReadMemU32)
        JLINKARM_ReadMemU32(0x20000000, 1, &val, NULL);

    printf("Memory [0x20000000] = 0x%08X\n", val);

    // Resume CPU and close
    if (JLINKARM_Go) {
        JLINKARM_Go();
    }

    JLINKARM_Close();
    dlclose(handle);

    printf("Closed.\n");
    return 0;
}
