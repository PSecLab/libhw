#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

struct hw_context {
    int sock;
};

static int hw_send(int sock, const char *cmd) {
    return write(sock, cmd, strlen(cmd));
}

static int hw_recv(int sock, char *buf, int buflen) {
    int total = 0;
    while (total < buflen - 1) {
        int n = read(sock, buf + total, 1);
        if (n <= 0) break;
        if (buf[total] == '\n') break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static int hw_cmd(int sock, const char *cmd, char *resp, int resplen) {
    if (hw_send(sock, cmd) < 0) return -1;
    if (resp && resplen > 0)
        return hw_recv(sock, resp, resplen);
    return 0;
}

hw_t* hw_connect(const char *host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(host),
    };

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return NULL;
    }

    hw_t *ctx = malloc(sizeof(hw_t));
    if (!ctx) {
        close(sock);
        return NULL;
    }

    ctx->sock = sock;
    return ctx;
}

void hw_close(hw_t *ctx) {
    if (!ctx) return;
    close(ctx->sock);
    free(ctx);
}

int hw_write32(hw_t *ctx, unsigned int addr, unsigned int value) {
    if (!ctx) return -1;
    char cmd[128], resp[128];
    snprintf(cmd, sizeof(cmd), "mww 0x%08X 0x%08X\n", addr, value);
    return hw_cmd(ctx->sock, cmd, resp, sizeof(resp)) < 0 ? -1 : 0;
}

int hw_read32(hw_t *ctx, unsigned int addr, unsigned int *value_out) {
    if (!ctx || !value_out) return -1;
    char cmd[128], resp[128];
    snprintf(cmd, sizeof(cmd), "mdw 0x%08X 1\n", addr);
    if (hw_cmd(ctx->sock, cmd, resp, sizeof(resp)) < 0)
        return -1;

    unsigned int tmp, val;
    if (sscanf(resp, "0x%X: %X", &tmp, &val) == 2) {
        *value_out = val;
        return 0;
    }
    return -1;
}

int hw_load_firmware(hw_t *ctx, const char *filename) {
    if (!ctx || !filename) return -1;
    char cmd[256], resp[256];

    // In OpenOCD, this uses: load_image <file> <address>
    // You might want to parameterize the address later.
    snprintf(cmd, sizeof(cmd), "load_image %s 0x08000000\n", filename);
    return hw_cmd(ctx->sock, cmd, resp, sizeof(resp)) < 0 ? -1 : 0;
}

