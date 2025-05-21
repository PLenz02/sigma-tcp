/**
 * Copyright (C) 2012 Analog Devices, Inc.
 *
 * THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, NON-INFRINGEMENT,
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *
 **/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdbool.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <ifaddrs.h> /* neu für getifaddrs() */
#include "sigma_tcp.h"

#define COMMAND_READ 0x0a
#define COMMAND_WRITE 0x0b

/* ------------------------------------------------------------------------
 * addr_to_str: wie vorher, um Sockaddr in String zu wandeln
 * ------------------------------------------------------------------------
static void addr_to_str(const struct sockaddr *sa, char *s, size_t maxlen)
{
    switch (sa->sa_family) {
    case AF_INET:
        inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen);
        break;
    case AF_INET6:
        inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen);
        break;
    default:
        strncpy(s, "Unknown", maxlen);
        break;
    }
}
*/
/* ------------------------------------------------------------------------
 * show_addrs: listet alle non-loopback Interfaces via getifaddrs()
 * ------------------------------------------------------------------------ */
static int show_addrs(int sck)
{
    struct ifaddrs *ifaddr, *ifa;
    char ip[INET6_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return 1;
    }

    printf("IP addresses:\n");
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        struct sockaddr *sa = ifa->ifa_addr;
        if (!sa || strcmp(ifa->ifa_name, "lo") == 0)
            continue;

        if (sa->sa_family == AF_INET) {
            inet_ntop(AF_INET,
                      &((struct sockaddr_in*)sa)->sin_addr,
                      ip, sizeof(ip));
        } else if (sa->sa_family == AF_INET6) {
            inet_ntop(AF_INET6,
                      &((struct sockaddr_in6*)sa)->sin6_addr,
                      ip, sizeof(ip));
        } else {
            continue;
        }

        printf(" %s: %s\n", ifa->ifa_name, ip);
    }
    freeifaddrs(ifaddr);
    return 0;
}

/* ------------------------------------------------------------------------
 * Debug-Backend (smolt)
 * ------------------------------------------------------------------------ */
static uint8_t debug_data[256];

static int debug_read(unsigned int addr, unsigned int len, uint8_t *data)
{
    if (addr < 0x4000 || addr + len > 0x4100) {
        memset(data, 0x00, len);
        return 0;
    }
    printf("read: %.2x %d\n", addr, len);
    memcpy(data, debug_data + (addr - 0x4000), len);
    return 0;
}

static int debug_write(unsigned int addr, unsigned int len, const uint8_t *data)
{
    if (addr < 0x4000 || addr + len > 0x4100)
        return 0;
    printf("write: %.2x %d\n", addr, len);
    memcpy(debug_data + (addr - 0x4000), data, len);
    return 0;
}

static const struct backend_ops debug_backend_ops = {
    .read = debug_read,
    .write = debug_write,
};

static const struct backend_ops *backend_ops = &debug_backend_ops;

/* ------------------------------------------------------------------------
 * Hilfsfunktionen für Netzwerk/Accept
 * ------------------------------------------------------------------------ */
static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return &(((struct sockaddr_in*)sa)->sin_addr);
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/* ------------------------------------------------------------------------
 * TCP-Protokoll-Handler
 * ------------------------------------------------------------------------ */
static void handle_connection(int fd)
{
    uint8_t *buf, *p;
    size_t buf_size = 256;
    unsigned int count = 0, len, addr;
    int ret;
    char command;

    buf = malloc(buf_size);
    if (!buf) goto exit;

    p = buf;
    while (1) {
        memmove(buf, p, count);
        p = buf + count;
        ret = read(fd, p, buf_size - count);
        if (ret <= 0)
            break;
        count += ret;
        p = buf;

        while (count >= 8) {
            command = p[0];
            len = (p[4] << 8) | p[5];
            addr = (p[6] << 8) | p[7];

            if (command == COMMAND_READ) {
                /* READ -> antworte mit WRITE-Frame + Daten */
                p += 8; count -= 8;
                buf[0] = COMMAND_WRITE;
                buf[1] = (0x4 + len) >> 8;
                buf[2] = (0x4 + len) & 0xff;
                buf[3] = backend_ops->read(addr, len, buf + 4);
                write(fd, buf, 4 + len);
            } else {
                /* WRITE-Frame empfangen */
                if (count < len + 8) {
                    if (buf_size < len + 8) {
                        buf_size = len + 8;
                        buf = realloc(buf, buf_size);
                        if (!buf) goto exit;
                    }
                    break;
                }
                backend_ops->write(addr, len, p + 8);
                p += len + 8;
                count -= len + 8;
            }
        }
    }

exit:
    free(buf);
}

/* ------------------------------------------------------------------------
 * main(): Backend wählen, Socket aufsetzen (Dual-Stack), Liste, Loop
 * ------------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int sockfd, new_fd;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr;
    socklen_t sin_size;
    int reuse = 1;
    char s[INET6_ADDRSTRLEN];
    int ret;

    /* Backend über CLI wählen */
    if (argc >= 2) {
        if (strcmp(argv[1], "debug") == 0) backend_ops = &debug_backend_ops;
        else if (strcmp(argv[1], "i2c") == 0) backend_ops = &i2c_backend_ops;
        else if (strcmp(argv[1], "regmap") == 0) backend_ops = &regmap_backend_ops;
        else {
            printf("Usage: %s <backend> <args>\nAvailable backends: debug, i2c, regmap\n", argv[0]);
            exit(0);
        }
        printf("Using %s backend\n", argv[1]);
    }
    /* Backend init (z.B. I2C device öffnen) */
    if (backend_ops->open && backend_ops->open(argc, argv))
        exit(1);

    /* Addrinfo für Listen-Socket */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; /* AF_INET + AF_INET6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; /* bind auf INADDR_ANY bzw. :: */

    if ((ret = getaddrinfo(NULL, "8086", &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return 1;
    }

    /* Durch alle addrinfo-Einträge laufen, bind + listen */
    for (p = servinfo; p; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) {
            perror("server: socket");
            continue;
        }

        /* Reuse-Flag, damit schneller restarten geht */
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            perror("setsockopt(SO_REUSEADDR)");
            exit(1);
        }

        /* Dual-Stack: IPv4-Verbindungen auf IPv6-Socket erlauben */
        if (p->ai_family == AF_INET6) {
            int off = 0;
            if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
                perror("setsockopt(IPV6_V6ONLY)");
                /* kein Fatal */
            }
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
            close(sockfd);
            perror("server: bind");
            continue;
        }
        break;
    }

    if (!p) {
        fprintf(stderr, "Failed to bind\n");
        return 2;
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, 1) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Waiting for connections...\n");
    show_addrs(sockfd);

    /* Accept-Loop */
    while (true) {
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd < 0) {
            perror("accept");
            continue;
        }
        inet_ntop(their_addr.ss_family,
                  get_in_addr((struct sockaddr *)&their_addr),
                  s, sizeof s);
        printf("New connection from %s\n", s);

        handle_connection(new_fd);

        printf("Connection closed\n");
        close(new_fd);
    }

    return 0;
}
