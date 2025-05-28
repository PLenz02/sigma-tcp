DESTDIR=/usr/local

CC = aarch64-linux-gnu-gcc

CFLAGS += -O2 -Wall -Werror -pedantic -std=gnu99 -Wno-unused-result --static

sigma_tcp: sigma_tcp.c i2c.c regmap.c
bioson_backend.c
	$(CC) $(CFLAGS) -o $@ $^

install:
	install -d $(DESTDIR)/bin
	install sigma_tcp $(DESTDIR)/bin

clean:
	rm -rf sigma_tcp *.o
