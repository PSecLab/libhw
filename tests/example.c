#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> // Required for clock_gettime

// This works because the Makefile provides the -Iinclude flag
#include "hw.h"

// A helper function to calculate the time difference in seconds.
static double get_time_diff(struct timespec *start, struct timespec *end) {
    return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1e9;
}

// Prints a usage message for the benchmarking tool.
static void print_usage(const char* prog_name) {
    fprintf(stderr, "\nUsage: %s <backend> <iterations>\n", prog_name);
    fprintf(stderr, "  Performs a benchmark of read and write operations.\n\n");
    fprintf(stderr, "  backend:    The hardware backend to use (e.g., 'stlink', 'openocd').\n");
    fprintf(stderr, "  iterations: The number of read/write operations to perform (e.g., 1000).\n\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* backend_name = argv[1];
    long num_iterations = atol(argv[2]);

    if (num_iterations <= 0) {
        fprintf(stderr, "Error: Number of iterations must be a positive integer.\n");
        return 1;
    }

    printf("--- Hardware Benchmarking Utility ---\n");
    printf("--> Attempting to connect with backend: '%s'...\n", backend_name);

    // Connect to the hardware.
    hw_t *hw = hw_connect(backend_name, NULL, 0);
    if (hw == NULL) {
        return 1; // hw_connect already printed an error.
    }

    // --- Prepare for Benchmark ---
    unsigned int address = 0x20000000; // SRAM start for many STM32s
    unsigned int value_to_write = 0xFEEDFACE;
    unsigned int value_read = 0;
    int status = 0;
    struct timespec start_time, end_time;

    // --- Benchmark Writes ---
    printf("--> Benchmarking %ld writes...\n", num_iterations);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (long i = 0; i < num_iterations; ++i) {
        status = hw_write32(hw, address, value_to_write);
        if (status != 0) {
            fprintf(stderr, "Write operation failed at iteration %ld!\n", i);
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double write_duration = get_time_diff(&start_time, &end_time);

    if (status != 0) {
        hw_close(hw);
        return 1;
    }

    // --- Benchmark Reads ---
    printf("--> Benchmarking %ld reads...\n", num_iterations);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (long i = 0; i < num_iterations; ++i) {
        status = hw_read32(hw, address, &value_read);
        if (status != 0) {
            fprintf(stderr, "Read operation failed at iteration %ld!\n", i);
            break;
        }
        // Verify correctness on every read
        if (value_read != value_to_write) {
            fprintf(stderr, "Data verification failed at iteration %ld! Read 0x%X, expected 0x%X\n",
                    i, value_read, value_to_write);
            status = -1;
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double read_duration = get_time_diff(&start_time, &end_time);

    // --- Print Final Report ---
    printf("\n--- Benchmark Complete ---\n");
    printf("Backend:      %s\n", backend_name);
    printf("Iterations:   %ld\n", num_iterations);
    printf("--------------------------\n");
    printf("Writes:\n");
    printf("  Total time:   %.4f seconds\n", write_duration);
    printf("  Avg time/op:  %.6f ms\n", (write_duration / num_iterations) * 1000.0);
    printf("  Ops/second:   %.2f\n", num_iterations / write_duration);
    printf("--------------------------\n");
    printf("Reads:\n");
    printf("  Total time:   %.4f seconds\n", read_duration);
    printf("  Avg time/op:  %.6f ms\n", (read_duration / num_iterations) * 1000.0);
    printf("  Ops/second:   %.2f\n", num_iterations / read_duration);
    printf("--------------------------\n");


    // --- Clean Up ---
    printf("\n--> Closing connection.\n");
    hw_close(hw);

    return (status == 0) ? 0 : 1;
}
