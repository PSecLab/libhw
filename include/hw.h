#ifndef HW_H
#define HW_H
#include <stdint.h>

// Pre-declare the primary opaque types used in the API.
typedef struct hw_context hw_t;
typedef struct hw_backend hw_backend_t;
typedef struct hw_ops hw_ops_t;

/**
 * The dispatch table (vtable) defines the complete set of operations
 * for a hardware backend "class", including its own constructor ('connect').
 */
struct hw_ops {
    /**
     * Establishes a connection to the hardware.
     * Allocates and returns a fully initialized hw_t context on success.
     * Returns NULL on failure.
     */
    hw_t* (*connect)(const char *host, int port);

    /**
     * Destroys a hardware context, cleanly closes the connection,
     * and frees all associated resources.
     */
    void (*close)(hw_t *ctx);

    /** Writes a 32-bit value to the target memory at the given address. */
    int (*write32)(hw_t *ctx, unsigned int addr, unsigned int value);

    /** Reads a 32-bit value from the target memory at the given address. */
    int (*read32)(hw_t *ctx, unsigned int addr, unsigned int *value_out);

	/* Tells if board is halted */
	int (*board_halted)(hw_t *ctx);

	int (*board_run)(hw_t *ctx);

	/* Read a register */
	uint64_t (*read_reg)(hw_t *ctx, int reg);

	/* Write to a register */
	void (*write_reg)(hw_t *ctx, int reg, uint64_t val);

};

/**
 * The generic hardware context, returned by connect() and passed to all
 * other hardware operations.
 */
struct hw_context {
    const hw_ops_t *ops; // Pointer to the backend's vtable.
    void *pvt_data;      // Pointer to the backend's private data.
};

/**
 * The descriptor for a pluggable backend.
 * This struct maps a name to a vtable of operations.
 */
struct hw_backend {
    const char *name;
    const hw_ops_t *ops;
};

/**
 * MACRO for compile-time registration of a backend.
 *
 * This places a pointer to the backend's descriptor into a special
 * linker section named ".hw_be_list". The linker will collect all such
 * pointers from all linked object files into a single, contiguous array.
 *
 * @param be_descriptor The hw_backend_t struct for the backend.
 */
#define HW_REGISTER_BACKEND(be_descriptor) \
    static const hw_backend_t* const __hw_be_ptr_##be_descriptor \
    __attribute__((__used__, __section__(".hw_be_list"))) = &be_descriptor


/* --- Public API Function Declarations --- */

/**
 * Connects to a hardware debugger by finding a registered backend by its name.
 *
 * @param backend_name The name of the backend to use (e.g., "stlink").
 * @param host For network backends, the hostname. Can be NULL.
 * @param port For network backends, the port number.
 * @return A pointer to a hardware context (hw_t), or NULL on failure.
 */
hw_t* hw_connect(const char *backend_name, const char *host, int port);

/** Wrapper for the backend's close function. */
void hw_close(hw_t *ctx);

/** Wrapper for the backend's write32 function. */
int hw_write32(hw_t *ctx, unsigned int addr, unsigned int value);

/** Wrapper for the backend's read32 function. */
int hw_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out);

/* Tells if board is halted */
int hw_board_halted(hw_t *ctx);

int hw_board_run(hw_t *ctx);

/* Read a register */
uint64_t hw_read_reg(hw_t *ctx, int reg);

/* Write to a register */
void hw_write_reg(hw_t *ctx, int reg, uint64_t val);

#endif // HW_H
