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

// --- The Publicly Visible Dispatch Table ---
const hw_ops_t openocd_ops = {
    .connect = openocd_impl_connect,
    .close   = openocd_impl_close,
    .write32 = openocd_impl_write32,
    .read32  = openocd_impl_read32,
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

    // Set a 5-second receive timeout on the socket. This is crucial.
    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    if (setsockopt(pvt->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
        perror("setsockopt failed"); close(pvt->sockfd); free(pvt); return NULL;
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
