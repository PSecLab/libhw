# libhw: A Pluggable Hardware Abstraction Layer

`libhw` is a lightweight C library designed to provide a unified interface for communicating with embedded hardware targets through various debugging probes. It features a clean, pluggable backend architecture that allows new probes and communication methods (like ST-Link, OpenOCD, J-Link, etc.) to be added with minimal effort.

The primary goal is to abstract away the specific details of each debugging tool, allowing a high-level application to perform memory read/write operations using a simple, consistent API.

## Features

- **Unified API:** Simple `hw_connect`, `hw_close`, `hw_read32`, and `hw_write32` functions for all backends.
- **Flash programming:** A common `hw_flash_*` API across every backend, including a differential `hw_flash_patch` that only touches the sectors that actually changed.
- **Pluggable Backends:** Easily add support for new debug probes by implementing a standard interface.
- **Central Registry:** A single, central file (`backends/hw_backends.c`) lists all available backends, making the system easy to understand and extend.
- **No External Build Dependencies:** Relies only on standard `make` and a C compiler.
- **Included Backends:**
  - `stlink`: Connects directly to ST-Link programmers via `libstlink`.
  - `openocd`: Connects to a running OpenOCD server via its TCL RPC interface (port 6666).
  - `mock`: An in-memory target with no hardware attached, used by the test suite.

## The Flash API

Every backend exposes the same flash interface. Addresses are absolute target
addresses (`0x08000000`, not an offset), and every call returns `0` on success
and `-1` on failure.

```c
hw_flash_info_t info;
hw_flash_info(hw, &info);      // base, size, erase and program granularity

hw_flash_read(hw, addr, buf, len);
hw_flash_erase(hw, addr, len); // erases whole sectors
hw_flash_write(hw, addr, buf, len);
hw_flash_mass_erase(hw);
hw_flash_verify(hw, addr, buf, len);
```

These are thin: they dispatch straight to the backend and do nothing else. In
particular `hw_flash_write` does **not** erase first, so on its own it can only
clear bits. Two composite calls, implemented once in `core/hw.c` on top of those
primitives, do the useful work:

### `hw_flash_update` — write an image

```c
hw_flash_update(hw, 0x08004000, image, image_len);
```

Erases and reprograms the range, then verifies it. Bytes that share a sector
with the range but fall outside it are read first and written back, so nothing
outside `[addr, addr + len)` changes.

### `hw_flash_patch` — write only what changed

```c
hw_flash_patch_stats_t stats;
hw_flash_patch(hw, 0x08004000, old_image, new_image, len, &stats);

printf("%u/%u sectors changed, %u erased, %u bytes written\n",
       stats.sectors_changed, stats.sectors_total,
       stats.sectors_erased, stats.bytes_written);
```

Given the image currently on the device and the one you want there, this applies
the minimal set of operations that gets from one to the other:

- A sector whose bytes are identical in both images is **never touched**, and
  with `old_image` supplied it is never even read back over the wire.
- A sector that did change is **erased only if some bit has to go 0 -> 1**.
  Programming can always clear bits, so a change that only clears them is
  written straight over the top, and only the differing span is reprogrammed
  (widened out to the part's program granularity).
- Otherwise the sector is erased and rewritten in full.

`old_image` may be `NULL`, in which case the current contents are read back and
used instead. That is always correct, but it costs a full read of the region;
passing the image you last flashed avoids it.

Both composite calls halt the target before programming and **leave it halted** —
resuming a core whose code just changed underneath it is the caller's decision.

### Backend notes

- **stlink** reports the real geometry from `libstlink`, and handles the
  non-uniform sector maps of F2/F4 and F7 (small sectors, then one medium, then
  large ones, repeating per 1 MB bank). Writes drive the flash loader directly
  rather than going through `stlink_write_flash`, which would erase every page
  it touched and defeat the point of `hw_flash_patch`.
- **openocd** reads its geometry from `flash banks` and `flash info 0`, so it
  gets real sector boundaries too. Two caveats: the TCL interface cannot carry a
  payload inline, so writes go through a temporary file that the OpenOCD server
  must be able to open (fine for a local server, which is the default; a remote
  one will not see it). And the interface does not report program granularity,
  so the backend conservatively reports the sector size — patches still skip
  erases, but they rewrite a changed sector whole.
- **mock** models NOR semantics faithfully: erase sets `0xFF`, programming only
  clears bits, and writes must respect the program granularity. Code that
  forgets to erase fails against it exactly as it would against a board.

## Testing

`make check` builds and runs `tests/flash_test.c` against the `mock` backend. It
needs no hardware:

```bash
make check
```

## Building the Project

### Prerequisites

- A C compiler (e.g., `gcc` or `clang`)
- GNU `make`
- `libstlink` development headers (e.g., `libstlink-dev` on Debian/Ubuntu)

### Compilation

From the root of the project directory, simply run `make`:

```bash
make
```

This will compile the `libhw.a` static library, the `hw_test` benchmark and the `flash_test` test suite, placing all output into the `out/` directory.

To clean up all build artifacts, run:

```bash
make clean
```

## Usage

The compiled `hw_test` utility can be used to perform benchmarks or simple memory operations.

**Syntax:**

```bash
./out/hw_test <backend> <iterations>
```

**Example 1: Benchmark the `stlink` backend with 1000 iterations**

```bash
./out/hw_test stlink 1000
```

**Example 2: Benchmark the `openocd` backend with 500 iterations**

First, ensure an OpenOCD server is running in a separate terminal:

```bash
# In terminal 1
openocd -f interface/st-link.cfg -f target/stm32f4x.cfg
```

Then, run the test utility:

```bash
# In terminal 2
./out/hw_test openocd 500
```

## How to Add a New Backend

Adding support for a new debug probe (e.g., "jlink") is straightforward.

1. **Create the Source File:**
   Create a new directory and C file for your backend, for example: `backends/jlink/hw_jlink.c`.

2. **Implement the Backend Logic:**
   Inside `hw_jlink.c`, you must implement the four static functions that define the backend's behavior:
   - `static hw_t* jlink_impl_connect(...)`
   - `static void jlink_impl_close(...)`
   - `static int jlink_impl_write32(...)`
   - `static int jlink_impl_read32(...)`

3. **Define the Operations Table (vtable):**
   In the same file (`hw_jlink.c`), create a publicly visible `hw_ops_t` struct that maps the generic operations to your static implementations.

   ```c
   // In hw_jlink.c
   const hw_ops_t jlink_ops = {
       .connect = jlink_impl_connect,
       .close   = jlink_impl_close,
       .write32 = jlink_impl_write32,
       .read32  = jlink_impl_read32,
   };
   ```

4. **Add to the Central Registry:**
   Open `backends/hw_backends.c` and add your new backend to the `g_known_backends` array.
   - First, add an `extern` declaration for your new ops table at the top of the file.
   - Then, add a new entry to the array itself.

   ```c
   // In backends/hw_backends.c
   
   // ... other externs ...
   extern const hw_ops_t jlink_ops; // Add this
   
   const backend_entry_t g_known_backends[] = {
       { "stlink",  &stlink_ops },
       { "openocd", &openocd_ops },
       { "jlink",   &jlink_ops }, // Add this line
       { NULL, NULL }
   };
   ```

5. **Update the Makefile:**
   Finally, open the `Makefile` and add your new source file's basename to the `LIB_SRC_NAMES` list. `make` will automatically find it and compile it into the library.

   ```makefile
   # In Makefile
   LIB_SRC_NAMES = hw.c \
                   hw_backends.c \
                   hw_stlink.c \
                   hw_openocd.c \
                   hw_jlink.c # Add this line
   ```

After these steps, run `make clean && make`, and your new backend will be fully integrated and available to use via the `hw_test` utility (e.g., `./out/hw_test jlink 1000`).

Disclaimer: Readme generated by Gemini.
