#
# Makefile for the Hardware Abstraction Layer (libhw)
#

# --- Tools and Flags ---
CC = gcc
# CFLAGS: Add include paths for public API, core, and backend headers
CFLAGS = -g -fPIC -Wall -Wextra -std=c11 -Iinclude -Icore -Ibackends -I/usr/include/stlink -I/usr/include/libusb-1.0/
LDFLAGS =
LIBS = -lstlink

# --- Project Structure ---
ODIR = out
TARGET = hw_test
LIBRARY = libhw.a

# VPATH: Tell 'make' where to look for source files.
# --- CORRECTED: Added backends/openocd to the search path ---
VPATH = core backends backends/stlink backends/openocd tests

# --- Source File Basenames ---
# List only the basenames of the source files. VPATH will find them.
LIB_SRC_NAMES = hw.c \
                hw_backends.c \
                hw_stlink.c \
                hw_openocd.c
APP_SRC_NAME = example.c

# --- Generated File Paths ---
LIB_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(LIB_SRC_NAMES))
APP_OBJ = $(patsubst %.c,$(ODIR)/%.o,$(APP_SRC_NAME))

# --- Header Files (for dependency tracking) ---
PUBLIC_HEADER = include/hw.h
PRIVATE_HEADER = core/hw_priv.h


# --- Build Rules ---
.PHONY: all clean

all: out/libhw.so $(ODIR)/$(TARGET)

# Rule to link the final executable
$(ODIR)/$(TARGET): $(APP_OBJ) $(ODIR)/$(LIBRARY)
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


# Rule to clean up all build artifacts
clean:
	@echo "Cleaning up..."
	rm -rf $(ODIR)
