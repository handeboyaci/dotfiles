CC ?= gcc
CFLAGS ?= -O3 -march=native -flto -fno-plt -fno-stack-protector -Wall -Wextra -I/usr/include/freetype2
LDFLAGS ?= -static -s
PREFIX ?= /usr
STATIC_LIBDIR := .static_libs

.PHONY: all clean install test

all: statusbar

$(STATIC_LIBDIR):
	mkdir -p $(STATIC_LIBDIR)
	for lib in libfreetype.a libpng16.a libz.a libbz2.a libbrotlidec.a libbrotlicommon.a libjson-c.a; do \
		objcopy --remove-section=.sframe /usr/lib/x86_64-linux-gnu/$$lib $(STATIC_LIBDIR)/$$lib; \
	done

statusbar: statusbar.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) statusbar.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -ljson-c -lz -lm -lpthread -o statusbar

test: statusbar
	./statusbar --self-test

install: statusbar
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 statusbar $(DESTDIR)$(PREFIX)/bin/statusbar

clean:
	rm -rf statusbar $(STATIC_LIBDIR)
