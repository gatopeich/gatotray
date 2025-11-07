### Generic Makefile for C code
#
#  (c) 2011, gatopeich, licensed under a Creative Commons Attribution 3.0
#  Unported License: http://creativecommons.org/licenses/by/3.0/
#  Briefly: Use it however suits you better and just give me due credit.
#
### Changelog:
# V4.1: Security fixes
# V4.0: Track top resource consuming processes
# V3.3: Add free memory chart (from proc/meminfo)
# V3.2: Smooth Screensaver render with Cairo
# V3.1: Bugfixes. Versioning
# V3.0: Screensaver support.
# V2.1: Added CCby license. Restructured a bit.
# V2.0: Added 32-bit target for 64 bits environment.

VERSION := 4.1
REL := $(shell git log -1 --format=%cd --date=format:%Y%m%d || date +%Y%m%d)
CFLAGS := -std=c11 -Wall -O2 -DNDEBUG -g2 -DVERSION=\"$(VERSION).$(REL)\" $(CFLAGS) -Wno-deprecated-declarations
CPPFLAGS := `pkg-config --cflags gtk+-2.0` $(CPPFLAGS)
LDLIBS := `pkg-config --libs gtk+-2.0` -lX11 $(LDLIBS)

$(warn $(DESTDIR))

targets := gatotray

all: $(targets)

gatotray: gatotray.o

gatotray.i386: gatotray.o.i386
	$(LD) -m32 -o $@ $^ $(LDLIBS)

install: gatotray gatotray.xpm gatotray.desktop gatotray-screensaver.desktop
	install -svDt $(DESTDIR)/usr/bin/ gatotray
	install -vDt $(DESTDIR)/usr/share/icons/ gatotray.xpm
	install -vDt $(DESTDIR)/usr/share/applications/ gatotray.desktop
	install -vDt $(DESTDIR)/usr/share/applications/screensavers/ gatotray-screensaver.desktop

deb: gatotray-$(VERSION).$(REL).deb
gatotray-$(VERSION).$(REL).deb: gatotray gatotray.xpm gatotray.desktop gatotray-screensaver.desktop Debian-Control
	install -vD gatotray root/usr/bin/gatotray
	strip root/usr/bin/gatotray
	install -vD gatotray.desktop root/usr/share/applications/gatotray.desktop
	install -vD gatotray-screensaver.desktop root/usr/share/applications/screensavers/gatotray-screensaver.desktop
	install -vD gatotray.xpm root/usr/share/icons/gatotray.xpm
	install -vD gatotray3X.png root/usr/share/icons/gatotray.png
	install -vD Debian-Control root/DEBIAN/control
	sed -i 's/^Version:.*/Version: $(VERSION)/' root/DEBIAN/control
	dpkg -b root $@

Debian-Control: Makefile
	sed -ie 's/^Version:.*/Version: $(VERSION).$(REL)/' $@

PKGBUILD: Makefile
	sed -ie 's/^pkgver=.*/pkgver=$(VERSION)/' $@
	sed -ie 's/^pkgrel=.*/pkgrel=$(REL)/' $@

# Tarball for building distribution packages
tarball: gatotray-$(VERSION).$(REL).tar.gz
gatotray-$(VERSION).$(REL).tar.gz: Debian-Control PKGBUILD
	git archive --prefix=gatotray-$(VERSION)/ -o $@ HEAD


### Magic rules follow

sources := $(targets:%=%.c)
objects := $(sources:.c=.o)
depends := $(sources:.c=.d)

clean:
	rm -f *.d *.o $(targets)

%.o: %.c %.d
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

%.i386: %.o.i386
	$(LD) -m32 -o $@ $^

%.o.i386: %.c %.d
	$(CC) -m32 -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

# Let %.o & %.d depend on %.c included files:
%.d: %.c
	$(CC) -MM $(CPPFLAGS) -MT $*.o -MT $@ -MF $@ $<

# Now include those freshly generated dependencies:
ifneq ($(MAKECMDGOALS),clean)
-include $(depends)
endif
