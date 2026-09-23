CC ?= gcc
CFLAGS ?= -O3 -march=native -flto -fno-plt -fno-stack-protector -Wall -Wextra -I/usr/include/freetype2
LDFLAGS ?= -static -s
PREFIX ?= /usr
STATIC_LIBDIR := .static_libs

.PHONY: all clean install test

all: statusbar pixelbuds-pause

$(STATIC_LIBDIR):
	mkdir -p $(STATIC_LIBDIR)
	for lib in libfreetype.a libpng16.a libz.a libbz2.a libbrotlidec.a libbrotlicommon.a libjson-c.a; do \
		objcopy --remove-section=.sframe /usr/lib/x86_64-linux-gnu/$$lib $(STATIC_LIBDIR)/$$lib; \
	done

statusbar: statusbar.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) statusbar.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -ljson-c -lz -lm -lpthread -o statusbar

pixelbuds-pause: pixelbuds-pause.c
	$(CC) -O2 -Wall -Wextra pixelbuds-pause.c $$(pkg-config --cflags --libs libsystemd) -o pixelbuds-pause

test: statusbar
	./statusbar --self-test

install: statusbar pixelbuds-pause
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 statusbar $(DESTDIR)$(PREFIX)/bin/statusbar
	install -m 755 pixelbuds-pause $(DESTDIR)$(PREFIX)/bin/pixelbuds-pause

clean:
	rm -rf statusbar pixelbuds-pause $(STATIC_LIBDIR)
