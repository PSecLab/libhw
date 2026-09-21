#
# Makefile for the Hardware Abstraction Layer (libhw)
#

# --- Tools and Flags ---
CC = gcc
# CFLAGS: Add include paths for public API, core, and backend headers
CFLAGS = -g -fPIC -Wall -Wextra -std=c11 -Iinclude -Icore -Ibackends -I/usr/include/stlink -I/usr/include/libusb-1.0/
CFLAGS += -Igenerated
CFLAGS += -I/usr/local/include/stlink	#in case, stlink is built from source.

LDFLAGS =
LIBS = -lstlink

# --- Project Structure ---
ODIR = out
TARGET = hw_test
TEST_TARGET = flash_test
PROBE_TARGET = state_probe
STATE_TEST_TARGET = state_test
LIBRARY = libhw.a

# VPATH: Tell 'make' where to look for source files.
# --- CORRECTED: Added backends/openocd to the search path ---
VPATH = core backends backends/stlink backends/openocd backends/mock tests

# --- Source File Basenames ---
# List only the basenames of the source files. VPATH will find them.
LIB_SRC_NAMES = hw.c \
                hw_backends.c \
                hw_stlink.c \
                hw_openocd.c \
                hw_mock.c \
                hw_state.c
APP_SRC_NAME = example.c
TEST_SRC_NAME = flash_test.c
PROBE_SRC_NAME = state_probe.c
STATE_TEST_SRC_NAME = state_test.c

# --- Generated File Paths ---
LIB_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(LIB_SRC_NAMES))
APP_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(APP_SRC_NAME))
TEST_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(TEST_SRC_NAME))
PROBE_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(PROBE_SRC_NAME))
STATE_TEST_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(STATE_TEST_SRC_NAME))

# --- Header Files (for dependency tracking) ---
PUBLIC_HEADER = include/hw.h
PRIVATE_HEADER = core/hw_priv.h


# --- Build Rules ---
.PHONY: all clean check state-import state-gen state-coverage state-check state-sweep state-prose

all: out/libhw.so $(ODIR)/$(TARGET) $(ODIR)/$(TEST_TARGET) $(ODIR)/$(PROBE_TARGET) $(ODIR)/$(STATE_TEST_TARGET)

# Rule to link the final executable
$(ODIR)/$(TARGET): $(APP_OBJ) $(ODIR)/$(LIBRARY)
	@echo "LD   ==> $@"
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

# Rule to link the flash test suite (runs against the mock backend)
$(ODIR)/$(TEST_TARGET): $(TEST_OBJ) $(ODIR)/$(LIBRARY)
	@echo "LD   ==> $@"
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

# Run the test suite. Needs no hardware.
check: $(ODIR)/$(TEST_TARGET) $(ODIR)/$(STATE_TEST_TARGET)
	@$(ODIR)/$(TEST_TARGET)
	@echo
	@$(ODIR)/$(STATE_TEST_TARGET)

# Rule to link the live-target state probe
$(ODIR)/$(PROBE_TARGET): $(PROBE_OBJ) $(ODIR)/$(LIBRARY)
	@echo "LD   ==> $@"
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

# Rule to link the hardware-free state database test
$(ODIR)/$(STATE_TEST_TARGET): $(STATE_TEST_OBJ) $(ODIR)/$(LIBRARY)
	@echo "LD   ==> $@"
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

# Rule to create the static library archive
$(ODIR)/$(LIBRARY): $(LIB_OBJ)
	@echo "AR   ==> $@"
	ar rcs $@ $^

# --- A Single, Generic Pattern Rule for Compilation ---
# This one rule can now build ALL object files thanks to VPATH.
# It finds the source file in the VPATH, compiles it, and puts the .o in $(ODIR).
# The dependency on both public and private headers ensures correctness.
$(ODIR)/%.o: %.c $(PUBLIC_HEADER) $(PRIVATE_HEADER)
	@echo "CC   ==> $<"
	@mkdir -p $(ODIR)
	$(CC) $(CFLAGS) -c $< -o $@


SHARED_LIB = libhw.so

$(ODIR)/$(SHARED_LIB): $(LIB_OBJ)
	@echo "LD   ==> $@"
	$(CC) -shared -fPIC -o $@ $^ $(LIBS)


# --- Architectural-state database -------------------------------------------
# The .def files under generated/ are outputs. To change what state libhw knows
# about, change the rules the importer reads and re-import; never edit the .def.
STATE_ARCH = armv7m
STATE_CPUS = cortex_m7_r0p2 cortex_m4_r0p0

# Re-import from the pinned manuals. Needs the licensed documents present.
state-import:
	@python3 tools/state_import.py --target $(STATE_ARCH)
	@for c in $(STATE_CPUS); do python3 tools/state_import.py --target $$c --arch-manifest $(STATE_ARCH); done

# Regenerate the .def tables and coverage artifacts from the manifests.
state-gen:
	@python3 tools/state_gen.py

# The accounting: every source entry classified, nothing unclassified.
state-coverage:
	@python3 tools/state_gen.py --coverage-only

# Sweep the manuals for state documented outside their register tables.
state-sweep:
	@python3 tools/state_prose_sweep.py --target $(STATE_ARCH)
	@for c in $(STATE_CPUS); do python3 tools/state_prose_sweep.py --target $$c; done

# Prose-state scan: every candidate classified, no unresolved ranges.
state-prose:
	@python3 tools/state_prose_candidates.py --target $(STATE_ARCH)
	@for c in $(STATE_CPUS); do python3 tools/state_prose_candidates.py --target $$c; done

# CI invariants for the state database.
state-check:
	@python3 -m pytest tests/state -q

# --- CI -----------------------------------------------------------------
# Everything that can be checked without a board and without the licensed Arm
# documents. The document-dependent checks skip rather than pass vacuously, and
# a skip is visible in the output where a silent success would not be.
#
# Checks that need hardware (out/state_probe) or the manuals (state-import,
# state-sweep, state-prose) are deliberately not part of this target.
.PHONY: ci
ci:
	@echo "== build (warnings are errors) =="
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory CFLAGS='$(CFLAGS) -Werror' all
	@echo
	@echo "== C tests against the mock backend =="
	@$(MAKE) --no-print-directory check
	@echo
	@echo "== state database invariants =="
	@python3 -m pytest tests/state -q -rs
	@echo
	@echo "== coverage report =="
	@python3 tools/state_gen.py --coverage-only

# Rule to clean up all build artifacts
clean:
	@echo "Cleaning up..."
	rm -rf $(ODIR)
