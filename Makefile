### Generic Makefile for C code
#
#  (c) 2011, gatopeich, licensed under a Creative Commons Attribution 3.0
#  Unported License: http://creativecommons.org/licenses/by/3.0/
#  Briefly: Use it however suits you better and just give me due credit.
#
### Changelog:
# V4.0: Track top resource consuming processes
# V3.3: Add free memory chart (from proc/meminfo)
# V3.2: Smooth Screensaver render with Cairo
# V3.1: Bugfixes. Versioning
# V3.0: Screensaver support.
# V2.1: Added CCby license. Restructured a bit.
# V2.0: Added 32-bit target for 64 bits environment.

VERSION := 4.0b
CFLAGS := -std=c11 -Wall -O2 -g2 -DVERSION=\"$(VERSION)\" $(CFLAGS) -Wno-deprecated-declarations
CPPFLAGS := `pkg-config --cflags gtk+-2.0` $(CPPFLAGS)
LDLIBS := `pkg-config --libs gtk+-2.0` $(LDLIBS)

targets := gatotray

all: $(targets)

gatotray: gatotray.o

gatotray.i386: gatotray.o.i386
	$(LD) -m32 -o $@ $^ $(LDLIBS)

install: gatotray
	strip $^
	install -vD $^ /usr/local/bin
	install -vD gatotray.xpm /usr/share/icons
	install -vD gatotray.desktop /usr/share/applications/gatotray.desktop

deb: gatotray-$(VERSION).deb
gatotray-$(VERSION).deb: gatotray gatotray.xpm gatotray.desktop Debian-Control
	install -vD gatotray root/usr/bin/gatotray
	strip root/usr/bin/gatotray
	install -vD gatotray.desktop root/usr/share/applications/gatotray.desktop
	install -vD gatotray.xpm root/usr/share/icons/gatotray.xpm
	sed -i 's/^Version:.*/Version: $(VERSION)/' Debian-Control
	install -vD Debian-Control root/DEBIAN/control
	dpkg -b root $@

# Additional: .api file for SciTE users...
.api: $(wildcard *.h)
	$(CC) -E $(CPPFLAGS) $^ |grep '('|sed 's/^[^[:space:]]*[[:space:]]\+//'|sort|uniq > $@


### Magic rules follow

sources := $(wildcard *.c)
objects := $(sources:.c=.o)
depends := $(sources:.c=.d)

clean:
	rm -f $(objects) $(depends) $(targets)

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
