
CFLAGS=-g -O2 -Wall

all: miid

clean:
	rm -f *~
	rm -f miid miid.o

dist: clean
	set -e; \
	cd ..; \
	tar zcfv miid.tar.gz --exclude=BitKeeper --exclude=SCCS miid-*

install: miid miid.8
	mkdir -p $(DESTDIR)/sbin
	mkdir -p $(DESTDIR)/usr/man/man8
	install -m 0755 miid $(DESTDIR)/sbin
	install -m 0644 miid.8 $(DESTDIR)/usr/man/man8
