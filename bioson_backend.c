/**
 * bioson_backend.c – ADI FIFO-basierter Zugriff auf A2B über /tmp/biosonlinux_fifo_in
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "sigma_tcp.h"

static int bioson_send_cmd(const char *cmd) {
    FILE *f = fopen("/tmp/biosonlinux_fifo_in", "w");
    if (!f) {
        perror("bioson: Failed to open FIFO");
        return -1;
    }
    fprintf(f, "%s\n", cmd);
    fclose(f);
    return 0;
}

static int bioson_open(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "bioson: Usage %s bioson <bus> <8bit-addr> <register>\n", argv[0]);
        return 1;
    }

    printf("Using bioson backend\n");
    return 0;
}

static int bioson_read(unsigned int reg, unsigned int len, uint8_t *data) {
    fprintf(stderr, "bioson: Read not supported (write-only backend)\n");
    return -1;
}

static int bioson_write(unsigned int reg, unsigned int len, const uint8_t *data) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "i 20 01 D8 %02X", reg); // Beispiel: Registeradresse im FIFO-Protokoll
    return bioson_send_cmd(cmd);
}

const struct backend_ops i2c_backend_ops = {
	.open = i2c_open,
	.read = i2c_read,
	.write = i2c_write,
};

const struct backend_ops bioson_backend_ops = {
	.open = bioson_open,
	.read = bioson_read,
	.write = bioson_write,
};
