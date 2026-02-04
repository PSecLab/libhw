/**
 * @file hw_openocd.c
 * @brief OpenOCD TCL RPC backend implementation for the hardware abstraction layer.
 *
 * This file contains the code to communicate with an OpenOCD server via its
 * TCL RPC interface (usually on port 6666). This is a structured, robust
 * protocol designed for machine-to-machine communication.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/tcp.h>

#include "hw.h"

// --- Private Data Structure ---
typedef struct {
    int sockfd;
} hw_openocd_pvt_t;

// --- Forward declarations ---
static hw_t* openocd_impl_connect(const char *host, int port);
static void openocd_impl_close(hw_t *ctx);
static int openocd_impl_write32(hw_t *ctx, unsigned int addr, unsigned int value);
static int openocd_impl_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out);
static uint64_t openocd_impl_read_reg(hw_t *ctx, int reg);
static void openocd_impl_write_reg(hw_t *ctx, int reg, uint64_t val);
static int openocd_impl_board_run(hw_t *ctx);
static int openocd_impl_board_halted(hw_t *ctx);
static int openocd_impl_write8(hw_t *ctx, unsigned int addr, uint8_t value);
static int openocd_tcl_exec(int sockfd, const char* cmd, char* response_buf, size_t response_len);

// --- Static Helper Functions ---
static void block_until_halted(hw_openocd_pvt_t *pvt);

static void block_until_halted(hw_openocd_pvt_t *pvt) {
    if (!pvt || pvt->sockfd < 0) return;

    char response[256];
    while (1) {
        if (openocd_tcl_exec(pvt->sockfd, "targets", response, sizeof(response)) != 0) {
            fprintf(stderr, "Failed to poll target state from OpenOCD.\n");
            return;
        }

        if (strstr(response, "halted")) {
            break; // Target is halted
        }
        usleep(10000); // Sleep 10ms before polling again (more responsive)
    }
}

// --- The Publicly Visible Dispatch Table ---
const hw_ops_t openocd_ops = {
    .connect = openocd_impl_connect,
    .close   = openocd_impl_close,
    .write32 = openocd_impl_write32,
    .read32  = openocd_impl_read32,
    .board_halted = openocd_impl_board_halted,
    .read_reg = openocd_impl_read_reg,
    .write_reg = openocd_impl_write_reg,
    .board_run = openocd_impl_board_run,
    .write8 = openocd_impl_write8,
};

// --- Helper Functions ---

/**
 * @brief Executes a TCL command and reads the response.
 *
 * This function sends a command terminated by '\x1a' and reads the response,
 * which is also terminated by '\x1a'. The socket must have a timeout set.
 * @return 0 on success, -1 on failure.
 */
static int openocd_tcl_exec(int sockfd, const char* cmd, char* response_buf, size_t response_len) {
    const char TCL_TERMINATOR = '\x1a';

    // Send the command followed by the TCL terminator.
    if (send(sockfd, cmd, strlen(cmd), 0) < 0 || send(sockfd, &TCL_TERMINATOR, 1, 0) < 0) {
        perror("send");
        return -1;
    }

    // Read the response. A single large read is safe due to the socket timeout.
    // We loop to handle fragmented packets, but expect to exit quickly.
    char temp_buf[1024] = {0};
    size_t total_bytes_read = 0;
    while(total_bytes_read < sizeof(temp_buf) -1) {
        ssize_t bytes_read = recv(sockfd, temp_buf + total_bytes_read, sizeof(temp_buf) - total_bytes_read - 1, 0);
        if (bytes_read < 0) {
            perror("recv");
            return -1;
        }
        if (bytes_read == 0) {
            fprintf(stderr, "OpenOCD connection closed.\n");
            return -1;
        }
        total_bytes_read += bytes_read;
        // Check if the terminator is in the received data
        if (memchr(temp_buf, TCL_TERMINATOR, total_bytes_read)) {
            break;
        }
    }

    // Find the terminator and replace it with a null byte to create a clean C string.
    char* terminator_pos = memchr(temp_buf, TCL_TERMINATOR, total_bytes_read);
    if (terminator_pos) {
        *terminator_pos = '\0';
    }

    strncpy(response_buf, temp_buf, response_len - 1);
    response_buf[response_len - 1] = '\0';

    return 0;
}


// --- Static Backend Function Implementations ---

static hw_t* openocd_impl_connect(const char *host, int port) {
    if (host == NULL) host = "127.0.0.1";
    if (port == 0) port = 6666; // Default OpenOCD TCL RPC port

    hw_openocd_pvt_t *pvt = calloc(1, sizeof(hw_openocd_pvt_t));
    if (!pvt) return NULL;

    pvt->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (pvt->sockfd < 0) { free(pvt); return NULL; }

    // Disable Nagle to reduce small-packet latency and set a short recv timeout.
    int flag = 1;
    if (setsockopt(pvt->sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)) < 0) {
        perror("setsockopt TCP_NODELAY failed"); close(pvt->sockfd); free(pvt); return NULL;
    }
    // Set a short receive timeout (200ms) to avoid blocking long on single-command replies.
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000; // 200ms
    if (setsockopt(pvt->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
        perror("setsockopt SO_RCVTIMEO failed"); close(pvt->sockfd); free(pvt); return NULL;
    }

    struct sockaddr_in serv_addr; memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET; serv_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        close(pvt->sockfd); free(pvt); return NULL;
    }
    if (connect(pvt->sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("OpenOCD TCL connection failed"); close(pvt->sockfd); free(pvt); return NULL;
    }

    // With TCL, there is no welcome banner. We can send a command immediately.
    char response_buf[256];
    if (openocd_tcl_exec(pvt->sockfd, "halt", response_buf, sizeof(response_buf)) != 0) {
        fprintf(stderr, "Failed to halt target via OpenOCD TCL\n");
        close(pvt->sockfd); free(pvt); return NULL;
    }

    hw_t *ctx = calloc(1, sizeof(hw_t));
    if (!ctx) { close(pvt->sockfd); free(pvt); return NULL; }

    ctx->ops = &openocd_ops;
    ctx->pvt_data = pvt;
    printf("OpenOCD backend connected via TCL to %s:%d and target halted.\n", host, port);
    return ctx;
}

static void openocd_impl_close(hw_t *ctx) {
    if (ctx == NULL) return;
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (pvt != NULL) {
        if (pvt->sockfd >= 0) {
            char buf[16];
            openocd_tcl_exec(pvt->sockfd, "resume", buf, sizeof(buf));
            close(pvt->sockfd);
        }
        free(pvt);
    }
    free(ctx);
}

static int openocd_impl_write32(hw_t *ctx, unsigned int addr, unsigned int value) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return -1;

    char command[128], response[128];
    snprintf(command, sizeof(command), "mww 0x%x 0x%x", addr, value);

    if (openocd_tcl_exec(pvt->sockfd, command, response, sizeof(response)) != 0) {
        return -1;
    }
    // A successful write returns an empty string. We don't need to check it.
    return 0;
}

static int openocd_impl_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0 || !value_out) return -1;

    char command[128], response[128];
    snprintf(command, sizeof(command), "mrw 0x%x", addr);

    if (openocd_tcl_exec(pvt->sockfd, command, response, sizeof(response)) != 0) {
        return -1;
    }

    // The response is now a clean string containing the value (e.g., "3735928559").
    // strtoul with base 0 automatically handles decimal or "0x" hex formats.
    *value_out = strtoul(response, NULL, 0);

    return 0;
}

static uint64_t openocd_impl_read_reg(hw_t *ctx, int reg) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return 0;

    char command[128], response[128];
    snprintf(command, sizeof(command), "reg %d", reg);

    openocd_tcl_exec(pvt->sockfd, "halt", response, sizeof(response));

    block_until_halted(pvt);

    if (openocd_tcl_exec(pvt->sockfd, command, response, sizeof(response)) != 0) {
        return 0;
    }

    // trim off the prefix to get just the hex.
    // Example response formats:
    // > reg 1
    // r1 (/32): 0xdeadbeef
    // We need to extract the part after the colon.
    char* colon_pos = strchr(response, ':');
    if (colon_pos) {
        // Move past the colon and any spaces
        char* value_str = colon_pos + 1;
        while (*value_str == ' ') value_str++;
        return strtoull(value_str, NULL, 0);
    }
    else {
        // Unexpected format
        return 0;
    }
}

static void openocd_impl_write_reg(hw_t *ctx, int reg, uint64_t val) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return;

    char command[128], response[128];
    snprintf(command, sizeof(command), "reg %d 0x%llx", reg, (unsigned long long)val);

    openocd_tcl_exec(pvt->sockfd, "halt", response, sizeof(response));

    block_until_halted(pvt);

    openocd_tcl_exec(pvt->sockfd, command, response, sizeof(response));
}

static int openocd_impl_board_run(hw_t *ctx) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return -1;

    char response[128];
    if (openocd_tcl_exec(pvt->sockfd, "resume", response, sizeof(response)) != 0) {
        return -1;
    }
    return 0;
}

static int openocd_impl_board_halted(hw_t *ctx) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return 0;

    char response[256];
    // TODO: Fix the exec function to show halted when we send the poll command.
    if (openocd_tcl_exec(pvt->sockfd, "targets", response, sizeof(response)) != 0) {
        return 0;
    }

    int halted = 0;
    if (strstr(response, "halted")) halted = 1;
    return halted;
}

static int openocd_impl_write8(hw_t *ctx, unsigned int addr, uint8_t value) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return -1;

    char command[128], response[128];
    snprintf(command, sizeof(command), "mwb 0x%x 0x%x", addr, value);

    if (openocd_tcl_exec(pvt->sockfd, command, response, sizeof(response)) != 0) {
        return -1;
    }
    return 0;
}

#ifdef TEST_OPENOCD_BACKEND

static int openocd_impl_halt_board(hw_t *ctx) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return -1;

    char response[128];
    if (openocd_tcl_exec(pvt->sockfd, "halt", response, sizeof(response)) != 0) {
        return -1;
    }

    return 0;
}

int main() {
    printf("[*] Connecting to OpenOCD Tcl at localhost:%d ...\n", 6666);
    hw_t* hw = openocd_impl_connect(NULL, 0);
    if (!hw) {
        fprintf(stderr, "[-] Failed to connect to OpenOCD Tcl.\n");
        return -1;
    }
    printf("[*] Connected successfully.\n");

    // The board should already be halted from the connect step.

    int halted = openocd_impl_board_halted(hw);
    if (!halted) {
        printf("[-] FAILED: Board is still running.\n");
        goto close;
    }
    printf("[+] Board is halted.\n");

    openocd_impl_board_run(hw);
    printf("[*] Resuming the board... ");
    halted = openocd_impl_board_halted(hw);
    if (halted) {
        printf("[-] FAILED: Board is still halted.\n");
        goto close;
    }
    printf("[+] Board is running.\n");

    // Verify that we can do proper writes and reads from registers

    //halt the board again
    openocd_impl_halt_board(hw);
    printf("[*] Board halted again for register test.\n");

    uint64_t original_pc = openocd_impl_read_reg(hw, 15); // Read PC
    printf("[*] Original PC: 0x%llx\n", (unsigned long long)original_pc);
    uint64_t new_pc = original_pc + 4;
    openocd_impl_write_reg(hw, 15, new_pc); // Write new PC
    printf("[*] Wrote new PC: 0x%llx\n", (unsigned long long)new_pc);
    uint64_t verify_pc = openocd_impl_read_reg(hw, 15); // Read back PC
    printf("[*] Verified PC: 0x%llx\n", (unsigned long long)verify_pc);
    if (verify_pc != new_pc) {
        printf("[-] FAILED: PC register write/read mismatch.\n");
        goto close;
    }
    printf("[+] PC register write/read successful.\n");

    // verify write8 and read32
    unsigned int test_addr = 0x20000000; // Example RAM address
    uint8_t test_value = 0xAB;
    if (openocd_impl_write8(hw, test_addr, test_value) != 0) {
        printf("[-] FAILED: write8 failed.\n");
        goto close;
    }
    unsigned int read_back = 0;
    if (openocd_impl_read32(hw, test_addr, &read_back) != 0) {
        printf("[-] FAILED: read32 failed.\n");
        goto close;
    }
    if ((read_back & 0xFF) != test_value) {
        printf("[-] FAILED: read32 value mismatch after write8.\n");
        goto close;
    }
    printf("[+] write8 and read32 successful. Value: 0x%02x\n", read_back & 0xFF);

    // resume board
    openocd_impl_board_run(hw);
    printf("[*] Resuming the board... ");
    halted = openocd_impl_board_halted(hw);
    if (halted) {
        printf("[-] FAILED: Board is still halted.\n");
        goto close;
    }
    printf("[+] Board is running.\n");

    sleep(1); // Let it run for a second

close:
    openocd_impl_close(hw);
    return 0;
}

#endif
