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
#include <limits.h>

#include "sigma_tcp.h"

#include <netinet/in.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <sys/ioctl.h>

#define MAX_ALLOWED_LEN (64 * 1024)

static void addr_to_str(const struct sockaddr *sa, char *s, size_t maxlen)
{
	switch(sa->sa_family) {
	case AF_INET:
		inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				s, maxlen);
		break;
	case AF_INET6:
		inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				s, maxlen);
		break;
	default:
		strncpy(s, "Unkown", maxlen);
		break;
	}
}

static int show_addrs(int sck)
{
	char buf[256];
	char ip[INET6_ADDRSTRLEN];
	struct ifconf ifc;
	struct ifreq *ifr;
	unsigned int i, n;
	int ret;

	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	ret = ioctl(sck, SIOCGIFCONF, &ifc);
	if (ret < 0) {
		perror("ioctl(SIOCGIFCONF)");
		return 1;
	}

	ifr = ifc.ifc_req;
	n = ifc.ifc_len / sizeof(struct ifreq);

	printf("IP addresses:\n");

	for (i = 0; i < n; i++) {
		struct sockaddr *addr = &ifr[i].ifr_addr;

		if (strcmp(ifr[i].ifr_name, "lo") == 0)
			continue;

		addr_to_str(addr, ip, INET6_ADDRSTRLEN);
		printf("%s: %s\n", ifr[i].ifr_name, ip);
	}

	return 0;
}

#define COMMAND_READ 0x0a
#define COMMAND_WRITE 0x0b

static uint8_t debug_data[256];

static int debug_read(unsigned int addr, unsigned int len, uint8_t *data)
{
	if (addr < 0x4000 || addr + len > 0x4100) {
		memset(data, 0x00, len);
		return 0;
	}

	printf("read: %.2x %d\n", addr, len);

	addr -= 0x4000;
	memcpy(data, debug_data + addr, len);

	return 0;
}

static int debug_write(unsigned int addr, unsigned int len, const uint8_t *data)
{
	if (addr < 0x4000 || addr + len > 0x4100)
		return 0;

	printf("write: %.2x %d\n", addr, len);

	addr -= 0x4000;
	memcpy(debug_data + addr, data, len);

	return 0;
}

static const struct backend_ops debug_backend_ops = {
	.read = debug_read,
	.write = debug_write,
};

static const struct backend_ops *backend_ops = &debug_backend_ops;

static void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
		return &(((struct sockaddr_in*)sa)->sin_addr);

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

static void handle_connection(int client)
{
 uint8_t *buf = NULL;
 size_t buf_size = 0;
 ssize_t count;
 uint8_t header[8];

 while (1) {
  // 1. Lese Header
  count = recv(client, header, sizeof(header), MSG_WAITALL);
  if (count <= 0)
   break;

  // 2. Extrahiere Länge
  uint32_t len = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);

  if (len > MAX_ALLOWED_LEN) {
	fprintf(stderr, "ERROR: Received length %u exceeds maximum allowed length %d\n",
			len, MAX_ALLOWED_LEN);
	goto exit;
  }
  
  // 3. Speichervergrößerung prüfen
  if (count < len + 8) {
   if (buf_size < len + 8) {
    size_t new_size = len + 8;
    if (buf_size != new_size) {
     fprintf(stderr, "DEBUG realloc: len = %u, new buf_size = %zu, count = %zd\n",
             len, new_size, count);
     fprintf(stderr, "BEFORE realloc: buf = %p\n", buf);
     
     void *tmp = realloc(buf, new_size);
     if (!tmp) {
      fprintf(stderr, "ERROR: realloc failed for size %zu\n", new_size);
      goto exit;
     }
     buf = tmp;
     buf_size = new_size;

     fprintf(stderr, "AFTER realloc: new buf = %p\n", buf);
    }
   }
  }

  // 4. Kopiere Header in Puffer
  memcpy(buf, header, 8);

  // 5. Lese restliche Nutzdaten
  count = recv(client, buf + 8, len, MSG_WAITALL);
  if (count <= 0)
   break;

  // 6. Übergib an das Backend
  if (backend_ops && backend_ops->write)
   backend_ops->write(buf[0], len, buf + 8);
 }

exit:
 free(buf);
 close(client);
}

int main(int argc, char *argv[])
{
    int sockfd, new_fd;
	struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr;
    socklen_t sin_size;
    int reuse = 1;
    char s[INET6_ADDRSTRLEN];
    int ret;

	if (argc >= 2) {
		if (strcmp(argv[1], "debug") == 0)
			backend_ops = &debug_backend_ops;
		else if (strcmp(argv[1], "i2c") == 0)
			backend_ops = &i2c_backend_ops;
		else if (strcmp(argv[1], "regmap") == 0)
			backend_ops = &regmap_backend_ops;
		else {
			printf("Usage: %s <backend> <backend arg0> ...\n"
				   "Available backends: debug, i2c, regmap\n", argv[0]);
			exit(0);
		}

		printf("Using %s backend\n", argv[1]);
	}

	if (backend_ops->open) {
		ret = backend_ops->open(argc, argv);
		if (ret)
			exit(1);
	}

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

	ret = getaddrinfo(NULL, "8086", &hints, &servinfo);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return 1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    if (p == NULL)  {
        fprintf(stderr, "Failed to bind\n");
        return 2;
    }

    freeaddrinfo(servinfo);

    if (listen(sockfd, SOMAXCONN) == -1) {
        perror("listen");
        exit(1);
    }

    printf("Waiting for connections...\n");
	show_addrs(sockfd);

    while (true) {
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("accept");
            continue;
        }

        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);

        printf("New connection from %s\n", s);
		handle_connection(new_fd);
        printf("Connection closed\n");
    }

    return 0;
}