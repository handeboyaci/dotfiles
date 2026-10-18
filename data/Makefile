CC ?= gcc
CFLAGS ?= -O3 -march=native -flto -fno-plt -fno-stack-protector -Wall -Wextra -I/usr/include/freetype2
LDFLAGS ?= -static -s
PREFIX ?= /usr
STATIC_LIBDIR := .static_libs

.PHONY: all clean install test

all: statusbar pixelbuds-pause meet-stream

$(STATIC_LIBDIR):
	mkdir -p $(STATIC_LIBDIR)
	for lib in libfreetype.a libpng16.a libz.a libbz2.a libbrotlidec.a libbrotlicommon.a libjson-c.a; do \
		objcopy --remove-section=.sframe /usr/lib/x86_64-linux-gnu/$$lib $(STATIC_LIBDIR)/$$lib; \
	done

statusbar: statusbar.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) statusbar.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -ljson-c -lz -lm -lpthread -o statusbar

statusbar-test: statusbar.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) -DENABLE_SELF_TEST $(LDFLAGS) statusbar.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -ljson-c -lz -lm -lpthread -o statusbar-test

meet-stream: meet-stream.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) meet-stream.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -lz -lm -lpthread -o meet-stream

pixelbuds-pause: pixelbuds-pause.c
	$(CC) -O2 -Wall -Wextra pixelbuds-pause.c $$(pkg-config --cflags --libs libsystemd) -o pixelbuds-pause

test: statusbar-test meet-stream
	./statusbar-test --self-test
	./meet-stream --self-test

install: statusbar pixelbuds-pause 09-timezone.sh
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 statusbar $(DESTDIR)$(PREFIX)/bin/statusbar
	install -m 755 pixelbuds-pause $(DESTDIR)$(PREFIX)/bin/pixelbuds-pause
	install -d $(DESTDIR)/etc/NetworkManager/dispatcher.d
	install -m 755 09-timezone.sh $(DESTDIR)/etc/NetworkManager/dispatcher.d/09-timezone

clean:
	rm -rf statusbar statusbar-test pixelbuds-pause meet-stream $(STATIC_LIBDIR)
