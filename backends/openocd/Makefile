# ======================================================
# Makefile to build hw_openocd.c
# ======================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -I../../include
LDFLAGS := 

TARGET  := hw_openocd
SRCS    := hw_openocd.c
OBJS    := $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
