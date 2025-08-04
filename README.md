# libhw: A Pluggable Hardware Abstraction Layer

`libhw` is a lightweight C library designed to provide a unified interface for communicating with embedded hardware targets through various debugging probes. It features a clean, pluggable backend architecture that allows new probes and communication methods (like ST-Link, OpenOCD, J-Link, etc.) to be added with minimal effort.

The primary goal is to abstract away the specific details of each debugging tool, allowing a high-level application to perform memory read/write operations using a simple, consistent API.

## Features

- **Unified API:** Simple `hw_connect`, `hw_close`, `hw_read32`, and `hw_write32` functions for all backends.
- **Pluggable Backends:** Easily add support for new debug probes by implementing a standard interface.
- **Central Registry:** A single, central file (`backends/hw_backends.c`) lists all available backends, making the system easy to understand and extend.
- **No External Build Dependencies:** Relies only on standard `make` and a C compiler.
- **Included Backends:**
  - `stlink`: Connects directly to ST-Link programmers via `libstlink`.
  - `openocd`: Connects to a running OpenOCD server via its TCL RPC interface (port 6666).

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

This will compile the `libhw.a` static library and the `hw_test` executable, placing all output into the `out/` directory.

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

