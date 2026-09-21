#ifndef HW_H
#define HW_H
#include <stdint.h>
#include <stddef.h>

// Pre-declare the primary opaque types used in the API.
typedef struct hw_context hw_t;
typedef struct hw_backend hw_backend_t;
typedef struct hw_ops hw_ops_t;

/**
 * Geometry of the target's program flash, filled in by hw_flash_info().
 *
 * 'page_size' is the *nominal* erase granularity. On parts with a uniform
 * layout it is the real one; on parts with non-uniform sectors (STM32 F2/F4/F7)
 * it is only a hint, and hw_flash_sector() must be used to find the true
 * boundaries of the erase block containing a given address.
 */
typedef struct {
    unsigned int base;        /* first address of the flash region */
    unsigned int size;        /* total flash size, in bytes */
    unsigned int page_size;   /* nominal erase granularity, in bytes */
    unsigned int write_align; /* program granularity, in bytes */
} hw_flash_info_t;

/**
 * Accounting for a hw_flash_patch() run, so callers can see how much of the
 * flash the patch actually touched. Optional: pass NULL if not needed.
 */
typedef struct {
    unsigned int sectors_total;   /* erase blocks spanned by the region */
    unsigned int sectors_changed; /* blocks whose contents differ */
    unsigned int sectors_erased;  /* blocks that needed an erase */
    unsigned int bytes_written;   /* bytes actually programmed */
} hw_flash_patch_stats_t;

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

    int (*write8)(hw_t *ctx, unsigned int addr, uint8_t value);

    /** Reads a 32-bit value from the target memory at the given address. */
    int (*read32)(hw_t *ctx, unsigned int addr, unsigned int *value_out);

    int (*read8)(hw_t *ctx, unsigned int addr, uint8_t *value_out);

	/* Tells if board is halted */
	int (*board_halted)(hw_t *ctx);

	int (*board_run)(hw_t *ctx);

	int (*board_halt)(hw_t *ctx);

	int (*board_step)(hw_t *ctx);

	/* Read a register */
	uint64_t (*read_reg)(hw_t *ctx, int reg);

	/* Write to a register */
	void (*write_reg)(hw_t *ctx, int reg, uint64_t val);

	/* Reset board */
	int (*board_reset)(hw_t *ctx);

	/* --- Flash operations ---
	 *
	 * Backends that cannot program flash simply leave these NULL; the
	 * hw_flash_* wrappers then fail with -1 rather than dispatching.
	 * None of these implicitly halt the target or erase before writing.
	 */

	/* Report the flash geometry. */
	int (*flash_info)(hw_t *ctx, hw_flash_info_t *info_out);

	/* Report the erase block containing 'addr'. Optional: when NULL the core
	 * assumes a uniform layout of flash_info()'s page_size. */
	int (*flash_sector)(hw_t *ctx, unsigned int addr,
	                    unsigned int *sector_base, unsigned int *sector_size);

	/* Bulk read from flash. Optional: when NULL the core falls back to
	 * read32/read8, which is correct but one round trip per word. */
	int (*flash_read)(hw_t *ctx, unsigned int addr, uint8_t *buf, size_t len);

	/* Program flash *without* erasing first. Bits can only go 1 -> 0. */
	int (*flash_write)(hw_t *ctx, unsigned int addr, const uint8_t *buf, size_t len);

	/* Erase every block overlapping [addr, addr + len). */
	int (*flash_erase)(hw_t *ctx, unsigned int addr, size_t len);

	/* Erase the whole chip. */
	int (*flash_mass_erase)(hw_t *ctx);
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

/** Wrapper for the backend's write32 function. */
int hw_write8(hw_t *ctx, unsigned int addr, uint8_t value);

/** Wrapper for the backend's read32 function. */
int hw_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out);

/** Wrapper for the backend's read8 function. */
int hw_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out);

/* Tells if board is halted */
int hw_board_halted(hw_t *ctx);

int hw_board_halt(hw_t *ctx);

int hw_board_step(hw_t *ctx);

int hw_board_run(hw_t *ctx);

/* Read a register */
uint64_t hw_read_reg(hw_t *ctx, int reg);

/* Write to a register */
void hw_write_reg(hw_t *ctx, int reg, uint64_t val);

/* Reset target board */
int hw_board_reset(hw_t *ctx);

/* --- Flash API ---------------------------------------------------------
 *
 * All of these return 0 on success and -1 on failure, and all of them
 * address flash by absolute target address (e.g. 0x08000000), not by offset.
 *
 * The four thin wrappers below dispatch straight to the backend and do
 * nothing else -- in particular hw_flash_write() does NOT erase first, so a
 * plain write can only clear bits. The composite calls (hw_flash_update,
 * hw_flash_patch) are implemented once in the core on top of them.
 */

/* Report the flash geometry (base, size, erase and program granularity). */
int hw_flash_info(hw_t *ctx, hw_flash_info_t *info_out);

/* Report the base and size of the erase block containing 'addr'. Handles the
 * non-uniform sector maps of STM32 F2/F4/F7; falls back to the uniform
 * page_size from hw_flash_info() for backends that do not implement it. */
int hw_flash_sector(hw_t *ctx, unsigned int addr,
                    unsigned int *sector_base, unsigned int *sector_size);

/* Read 'len' bytes of flash into 'buf'. */
int hw_flash_read(hw_t *ctx, unsigned int addr, void *buf, size_t len);

/* Program 'len' bytes at 'addr'. Does not erase: the affected blocks must
 * already be erased, or the write must only clear bits. */
int hw_flash_write(hw_t *ctx, unsigned int addr, const void *buf, size_t len);

/* Erase every block overlapping [addr, addr + len). Note that this erases
 * whole blocks, so it can destroy data outside the requested range --
 * hw_flash_update() and hw_flash_patch() preserve those bytes for you. */
int hw_flash_erase(hw_t *ctx, unsigned int addr, size_t len);

/* Erase the entire flash. */
int hw_flash_mass_erase(hw_t *ctx);

/* Read back 'len' bytes at 'addr' and compare against 'buf'.
 * Returns 0 if they match, -1 otherwise. */
int hw_flash_verify(hw_t *ctx, unsigned int addr, const void *buf, size_t len);

/* Erase-and-reprogram [addr, addr + len) with 'buf', then verify.
 *
 * Bytes that share an erase block with the region but fall outside it are
 * read first and written back, so nothing outside [addr, addr + len) changes.
 * Halts the target before programming and leaves it halted.
 */
int hw_flash_update(hw_t *ctx, unsigned int addr, const void *buf, size_t len);

/* Apply the minimal set of flash operations that turns 'old_image' into
 * 'new_image' over [addr, addr + len), then verify.
 *
 * Erase blocks whose bytes are identical in the two images are left entirely
 * untouched. For a block that did change, the erase is skipped when every
 * differing bit only goes 1 -> 0, in which case just the differing span is
 * reprogrammed; otherwise the block is erased and rewritten in full.
 *
 * 'old_image' may be NULL, in which case the current flash contents are read
 * back and used as the old image -- correct, but it costs a full read of the
 * region. Passing the image you last flashed avoids that.
 *
 * Both images are 'len' bytes long and describe the same address range.
 * 'stats_out' may be NULL. Halts the target and leaves it halted.
 */
int hw_flash_patch(hw_t *ctx, unsigned int addr,
                   const void *old_image, const void *new_image, size_t len,
                   hw_flash_patch_stats_t *stats_out);

#endif // HW_H
