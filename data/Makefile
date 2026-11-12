CC ?= gcc
CFLAGS ?= -O3 -march=native -flto -fno-plt -fno-stack-protector -Wall -Wextra -I/usr/include/freetype2
LDFLAGS ?= -static -s
PREFIX ?= /usr
STATIC_LIBDIR := .static_libs

.PHONY: all clean install test

all: statusbar bt-media-helper fullmoon

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

bt-media-helper: bt-media-helper.c
	$(CC) -O2 -Wall -Wextra bt-media-helper.c $$(pkg-config --cflags --libs libsystemd) -o bt-media-helper

pixelbuds-pause: bt-media-helper
	ln -sf bt-media-helper pixelbuds-pause

test: statusbar-test fullmoon
	./statusbar-test --self-test
	./fullmoon --self-test

install: statusbar bt-media-helper 09-timezone.sh 99-audio-leds.rules
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 statusbar $(DESTDIR)$(PREFIX)/bin/statusbar
	install -m 755 bt-media-helper $(DESTDIR)$(PREFIX)/bin/bt-media-helper
	ln -sf bt-media-helper $(DESTDIR)$(PREFIX)/bin/pixelbuds-pause
	install -d $(DESTDIR)/etc/NetworkManager/dispatcher.d
	install -m 755 09-timezone.sh $(DESTDIR)/etc/NetworkManager/dispatcher.d/09-timezone
	install -d $(DESTDIR)/etc/udev/rules.d
	install -m 644 99-audio-leds.rules $(DESTDIR)/etc/udev/rules.d/99-audio-leds.rules

clean:
	rm -rf statusbar statusbar-test bt-media-helper pixelbuds-pause fullmoon $(STATIC_LIBDIR)
