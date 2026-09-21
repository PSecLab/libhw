#include "hw.h" // We need the hw_ops_t type definition
#include "hw_backends.h"
#include <stddef.h> // For NULL


// =============================================================================
//               --- To add a new backend, follow these steps ---
//
// 1. Add an 'extern' declaration for your backend's global ops table below.
// 2. Add a new entry to the 'g_known_backends' array.
// 3. Add your backend's source file to the Makefile's LIB_SRC list.
//
// =============================================================================

// --- Step 1: Extern declarations for all backend vtables ---
extern const hw_ops_t stlink_ops;
extern const hw_ops_t openocd_ops;
extern const hw_ops_t mock_ops;

// --- Step 2: The global, central registry of all known backends ---
const backend_entry_t g_known_backends[] = {
    { "stlink", &stlink_ops },
    { "openocd", &openocd_ops },
    { "mock", &mock_ops },
    { NULL, NULL }
};

