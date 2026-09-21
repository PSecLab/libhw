/**
 * @file hw_openocd.c
 * @brief OpenOCD TCL RPC backend implementation for the hardware abstraction layer.
 *
 * This file contains the code to communicate with an OpenOCD server via its
 * TCL RPC interface (usually on port 6666). This is a structured, robust
 * protocol designed for machine-to-machine communication.
 */

#define _DEFAULT_SOURCE

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
#include <ctype.h>

#include "hw.h"

// --- Private Data Structure ---
typedef struct {
    int sockfd;
    // Bank 0 geometry, cached after the first "flash banks" query so that
    // walking a range sector by sector does not re-ask on every step.
    unsigned int flash_base;
    unsigned int flash_size;
    int flash_known;
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
static int openocd_impl_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out);
static int openocd_tcl_exec(int sockfd, const char* cmd, char* response_buf, size_t response_len);
static int openocd_tcl_exec_alloc(int sockfd, const char* cmd, char **response_out);
static int openocd_impl_flash_info(hw_t *ctx, hw_flash_info_t *info_out);
static int openocd_impl_flash_sector(hw_t *ctx, unsigned int addr,
                                     unsigned int *sector_base, unsigned int *sector_size);
static int openocd_impl_flash_read(hw_t *ctx, unsigned int addr, uint8_t *buf, size_t len);
static int openocd_impl_flash_write(hw_t *ctx, unsigned int addr, const uint8_t *buf, size_t len);
static int openocd_impl_flash_erase(hw_t *ctx, unsigned int addr, size_t len);
static int openocd_impl_flash_mass_erase(hw_t *ctx);

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
    .read8  = openocd_impl_read8,
    .flash_info = openocd_impl_flash_info,
    .flash_sector = openocd_impl_flash_sector,
    .flash_read = openocd_impl_flash_read,
    .flash_write = openocd_impl_flash_write,
    .flash_erase = openocd_impl_flash_erase,
    .flash_mass_erase = openocd_impl_flash_mass_erase,
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

static int openocd_impl_read8(hw_t *ctx, unsigned int addr, uint8_t *value_out) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0 || !value_out) return -1;

    char command[128], response[128];
    snprintf(command, sizeof(command), "mrb 0x%x", addr);

    if (openocd_tcl_exec(pvt->sockfd, command, response, sizeof(response)) != 0) {
        return -1;
    }

    *value_out = (uint8_t)strtoul(response, NULL, 0);
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

// --- Flash operations ---

/**
 * @brief Like openocd_tcl_exec(), but for replies of unbounded length.
 *
 * A sector listing or a bulk 'mdw' runs to several kilobytes, well past the
 * fixed 1 KB buffer the ordinary exec path uses. On success the caller owns
 * the returned buffer and must free() it.
 */
static int openocd_tcl_exec_alloc(int sockfd, const char* cmd, char **response_out) {
    const char TCL_TERMINATOR = '\x1a';
    *response_out = NULL;

    if (send(sockfd, cmd, strlen(cmd), 0) < 0 || send(sockfd, &TCL_TERMINATOR, 1, 0) < 0) {
        perror("send");
        return -1;
    }

    size_t cap = 4096, used = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;

    for (;;) {
        if (used + 1 >= cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) { free(buf); return -1; }
            buf = bigger;
            cap *= 2;
        }

        ssize_t bytes_read = recv(sockfd, buf + used, cap - used - 1, 0);
        if (bytes_read < 0) { perror("recv"); free(buf); return -1; }
        if (bytes_read == 0) {
            fprintf(stderr, "OpenOCD connection closed.\n");
            free(buf);
            return -1;
        }
        used += (size_t)bytes_read;

        char *terminator = memchr(buf, TCL_TERMINATOR, used);
        if (terminator) {
            *terminator = '\0';
            *response_out = buf;
            return 0;
        }
    }
}

/**
 * @brief Best-effort failure detection for a TCL reply.
 *
 * The TCL RPC channel carries no status code -- a command that fails simply
 * returns its error text -- so scanning for the usual markers is all we can do.
 */
static int openocd_reply_failed(const char *reply) {
    static const char *markers[] = { "error", "fail", "invalid", "not found" };

    for (const char *p = reply; *p; p++) {
        for (unsigned int m = 0; m < sizeof(markers) / sizeof(markers[0]); m++) {
            size_t n = strlen(markers[m]), k = 0;
            while (k < n && p[k] && tolower((unsigned char)p[k]) == markers[m][k]) k++;
            if (k == n) return 1;
        }
    }
    return 0;
}

/**
 * @brief Base and size of flash bank 0, from "flash banks".
 *
 * The reply looks like:
 *   #0 : stm32f1x.flash (stm32f1x) at 0x08000000, size 0x00010000, buswidth 0, ...
 *
 * Only bank 0 is considered: the abstraction exposes a single flat flash
 * region, so a target with several banks is described by its first one.
 */
static int openocd_flash_bank(hw_openocd_pvt_t *pvt) {
    if (pvt->flash_known) return 0;

    char *reply = NULL;
    if (openocd_tcl_exec_alloc(pvt->sockfd, "flash banks", &reply) != 0) return -1;

    int rc = -1;
    const char *at   = strstr(reply, " at 0x");
    const char *size = strstr(reply, "size 0x");
    if (at && size) {
        pvt->flash_base = (unsigned int)strtoul(at + 4, NULL, 0);
        pvt->flash_size = (unsigned int)strtoul(size + 5, NULL, 0);
        if (pvt->flash_size > 0) {
            pvt->flash_known = 1;
            rc = 0;
        }
    }

    if (rc != 0) {
        fprintf(stderr, "Could not parse OpenOCD's 'flash banks' reply: %s\n", reply);
    }
    free(reply);
    return rc;
}

static int openocd_impl_flash_sector(hw_t *ctx, unsigned int addr,
                                     unsigned int *sector_base, unsigned int *sector_size) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0 || !sector_base || !sector_size) return -1;
    if (openocd_flash_bank(pvt) != 0) return -1;
    if (addr < pvt->flash_base || (addr - pvt->flash_base) >= pvt->flash_size) return -1;

    char *reply = NULL;
    if (openocd_tcl_exec_alloc(pvt->sockfd, "flash info 0", &reply) != 0) return -1;

    // Sector lines look like:  #  3: 0x00000c00 (0x400 1kB) not protected
    // The bank header on the first line has no '(' after its address, which is
    // what keeps it from being mistaken for a sector.
    unsigned int want_offset = addr - pvt->flash_base;
    int rc = -1;

    for (char *line = strtok(reply, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *colon = strchr(line, ':');
        if (!colon || strchr(line, '#') == NULL) continue;

        char *offset_str = strstr(colon, "0x");
        if (!offset_str) continue;
        char *paren = strchr(offset_str, '(');
        if (!paren) continue;
        char *size_str = strstr(paren, "0x");
        if (!size_str) continue;

        unsigned int offset = (unsigned int)strtoul(offset_str, NULL, 0);
        unsigned int size   = (unsigned int)strtoul(size_str, NULL, 0);
        if (size == 0) continue;

        if (want_offset >= offset && want_offset - offset < size) {
            *sector_base = pvt->flash_base + offset;
            *sector_size = size;
            rc = 0;
            break;
        }
    }

    if (rc != 0) {
        fprintf(stderr, "No sector covering 0x%08x in OpenOCD's 'flash info 0'.\n", addr);
    }
    free(reply);
    return rc;
}

static int openocd_impl_flash_info(hw_t *ctx, hw_flash_info_t *info_out) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0 || !info_out) return -1;
    if (openocd_flash_bank(pvt) != 0) return -1;

    unsigned int sector_base = 0, sector_size = 0;
    if (openocd_impl_flash_sector(ctx, pvt->flash_base, &sector_base, &sector_size) != 0) {
        return -1;
    }

    info_out->base = pvt->flash_base;
    info_out->size = pvt->flash_size;
    info_out->page_size = sector_size;

    // The TCL interface does not report program granularity, and it differs by
    // family (2, 4, 8 or 32 bytes). Reporting the sector size is the only safe
    // answer: it makes hw_flash_patch() rewrite a changed sector whole rather
    // than risk a misaligned partial program. The erase is still skipped when
    // the change only clears bits, which is where most of the cost sits.
    info_out->write_align = sector_size;
    return 0;
}

// 256 words per 'mdw' keeps each reply to a few KB.
#define OPENOCD_READ_WORDS 256u

static int openocd_impl_flash_read(hw_t *ctx, unsigned int addr, uint8_t *buf, size_t len) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0 || !buf) return -1;

    size_t done = 0;
    while (done < len) {
        // 'mdw' reads whole words from a word-aligned address, so read the
        // enclosing aligned window and copy the part the caller asked for.
        unsigned int cur     = addr + (unsigned int)done;
        unsigned int aligned = cur & ~3u;
        unsigned int skip    = cur - aligned;

        size_t want  = len - done;
        size_t bytes = want + skip;
        if (bytes > OPENOCD_READ_WORDS * 4) bytes = OPENOCD_READ_WORDS * 4;
        unsigned int words = (unsigned int)((bytes + 3) / 4);

        char cmd[64];
        char *reply = NULL;
        snprintf(cmd, sizeof(cmd), "mdw 0x%x %u", aligned, words);
        if (openocd_tcl_exec_alloc(pvt->sockfd, cmd, &reply) != 0) return -1;

        uint8_t *raw = malloc((size_t)words * 4);
        if (!raw) { free(reply); return -1; }

        // Each line is "0xADDRESS: w0 w1 w2 w3"; take the words in order.
        unsigned int n = 0;
        for (char *line = strtok(reply, "\r\n"); line && n < words; line = strtok(NULL, "\r\n")) {
            char *p = strchr(line, ':');
            if (!p) continue;
            p++;

            while (n < words) {
                while (*p == ' ' || *p == '\t') p++;
                if (!isxdigit((unsigned char)*p)) break;

                uint32_t word = (uint32_t)strtoul(p, &p, 16);
                raw[n * 4 + 0] = (uint8_t)(word & 0xFF);
                raw[n * 4 + 1] = (uint8_t)((word >> 8) & 0xFF);
                raw[n * 4 + 2] = (uint8_t)((word >> 16) & 0xFF);
                raw[n * 4 + 3] = (uint8_t)((word >> 24) & 0xFF);
                n++;
            }
        }
        free(reply);

        if (n < words) {
            fprintf(stderr, "OpenOCD returned %u of %u words for a read at 0x%08x\n",
                    n, words, aligned);
            free(raw);
            return -1;
        }

        size_t copy = (size_t)words * 4 - skip;
        if (copy > want) copy = want;
        memcpy(buf + done, raw + skip, copy);
        free(raw);
        done += copy;
    }
    return 0;
}

/**
 * @brief Program flash through OpenOCD's "flash write_image".
 *
 * The TCL interface has no way to carry a payload inline, so the data goes via
 * a temporary file. That means the OpenOCD server has to be able to open the
 * path: fine when it runs on this machine, which is the default and the usual
 * case, but a server on another host will not see the file. Note also that
 * write_image does not erase, which is exactly the contract this op wants.
 */
static int openocd_impl_flash_write(hw_t *ctx, unsigned int addr, const uint8_t *buf, size_t len) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0 || !buf) return -1;
    if (len == 0) return 0;

    char path[] = "/tmp/libhw_flash_XXXXXX";
    char cmd[256];
    char reply[512];
    FILE *f = NULL;
    int rc = -1;

    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return -1; }

    f = fdopen(fd, "wb");
    if (!f) { perror("fdopen"); close(fd); goto cleanup; }
    if (fwrite(buf, 1, len, f) != len) { perror("fwrite"); fclose(f); goto cleanup; }
    if (fclose(f) != 0) { perror("fclose"); goto cleanup; }

    snprintf(cmd, sizeof(cmd), "flash write_image %s 0x%x bin", path, addr);
    if (openocd_tcl_exec(pvt->sockfd, cmd, reply, sizeof(reply)) != 0) goto cleanup;
    if (openocd_reply_failed(reply)) {
        fprintf(stderr, "OpenOCD refused a flash write of %zu bytes at 0x%08x: %s\n",
                len, addr, reply);
        goto cleanup;
    }
    rc = 0;

cleanup:
    unlink(path);
    return rc;
}

static int openocd_impl_flash_erase(hw_t *ctx, unsigned int addr, size_t len) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return -1;
    if (len == 0) return 0;

    // erase_address wants a sector-aligned range; the core hands us exact
    // sector bounds, so no padding flag is needed (and none is wanted -- 'pad'
    // would quietly erase past the range it was given).
    char cmd[128], reply[512];
    snprintf(cmd, sizeof(cmd), "flash erase_address 0x%x %zu", addr, len);

    if (openocd_tcl_exec(pvt->sockfd, cmd, reply, sizeof(reply)) != 0) return -1;
    if (openocd_reply_failed(reply)) {
        fprintf(stderr, "OpenOCD refused a flash erase of %zu bytes at 0x%08x: %s\n",
                len, addr, reply);
        return -1;
    }
    return 0;
}

static int openocd_impl_flash_mass_erase(hw_t *ctx) {
    hw_openocd_pvt_t *pvt = (hw_openocd_pvt_t*)ctx->pvt_data;
    if (!pvt || pvt->sockfd < 0) return -1;

    char reply[512];
    if (openocd_tcl_exec(pvt->sockfd, "flash erase_sector 0 0 last", reply, sizeof(reply)) != 0) {
        return -1;
    }
    if (openocd_reply_failed(reply)) {
        fprintf(stderr, "OpenOCD refused a mass erase: %s\n", reply);
        return -1;
    }
    return 0;
}
