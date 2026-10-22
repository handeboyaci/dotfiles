CC ?= gcc
CFLAGS ?= -O3 -march=native -flto -fno-plt -fno-stack-protector -Wall -Wextra -I/usr/include/freetype2
LDFLAGS ?= -static -s
PREFIX ?= /usr
STATIC_LIBDIR := .static_libs

.PHONY: all clean install test

all: statusbar pixelbuds-pause fullmoon

$(STATIC_LIBDIR):
	mkdir -p $(STATIC_LIBDIR)
	for lib in libfreetype.a libpng16.a libz.a libbz2.a libbrotlidec.a libbrotlicommon.a libjson-c.a; do \
		objcopy --remove-section=.sframe /usr/lib/x86_64-linux-gnu/$$lib $(STATIC_LIBDIR)/$$lib; \
	done

statusbar: statusbar.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) statusbar.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -ljson-c -lz -lm -lpthread -o statusbar

statusbar-test: statusbar.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) -DENABLE_SELF_TEST $(LDFLAGS) statusbar.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -ljson-c -lz -lm -lpthread -o statusbar-test

fullmoon: fullmoon.c $(STATIC_LIBDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) fullmoon.c -L$(STATIC_LIBDIR) -lfreetype -lpng16 -lbz2 -lbrotlidec -lbrotlicommon -lz -lm -lpthread -o fullmoon

pixelbuds-pause: pixelbuds-pause.c
	$(CC) -O2 -Wall -Wextra pixelbuds-pause.c $$(pkg-config --cflags --libs libsystemd) -o pixelbuds-pause

test: statusbar-test fullmoon
	./statusbar-test --self-test
	./fullmoon --self-test

install: statusbar pixelbuds-pause 09-timezone.sh 99-audio-leds.rules
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 statusbar $(DESTDIR)$(PREFIX)/bin/statusbar
	install -m 755 pixelbuds-pause $(DESTDIR)$(PREFIX)/bin/pixelbuds-pause
	install -d $(DESTDIR)/etc/NetworkManager/dispatcher.d
	install -m 755 09-timezone.sh $(DESTDIR)/etc/NetworkManager/dispatcher.d/09-timezone
	install -d $(DESTDIR)/etc/udev/rules.d
	install -m 644 99-audio-leds.rules $(DESTDIR)/etc/udev/rules.d/99-audio-leds.rules

clean:
	rm -rf statusbar statusbar-test pixelbuds-pause fullmoon $(STATIC_LIBDIR)
