### Generic Makefile for C code
#
#  (c) 2011, gatopeich, licensed under a Creative Commons Attribution 3.0
#  Unported License: http://creativecommons.org/licenses/by/3.0/
#  Briefly: Use it however suits you better and just give me due credit.
#
### Changelog:
# V2.1: Added CCby license. Restructured a bit.
# V2.0: Added 32-bit target for 64 bits environment.

CFLAGS := -std=c99 -Wall -O3 -g3 $(CFLAGS)
CPPFLAGS := `pkg-config --cflags gtk+-2.0` $(CPPFLAGS)
LDLIBS := `pkg-config --libs gtk+-2.0` $(LDLIBS)

targets := gatotray

all: $(targets)

gatotray: gatotray.o

gatotray.bin32: gatotray.o32
	$(LD) $(LDLIBS) -m32 -o $@ $^

install: gatotray
	strip $^
	install -vD $^ /usr/bin
	install -vD gatotray.xpm /usr/share/icons
	install -vD xgatotray.desktop /usr/share/applications/screensavers/xgatotray.desktop

gatotray.deb: gatotray gatotray.xpm xgatotray.desktop Debian-Control
	strip gatotray
	#install -vD gatotray root/opt/extras.ubuntu.com/gatotray/gatotray
	install -vD gatotray root/usr/bin/gatotray
	install -vD xgatotray.desktop root/usr/share/applications/screensavers/xgatotray.desktop
	install -vD gatotray.xpm root/usr/share/icons/gatotray.xpm
	install -vD Debian-Control root/DEBIAN/control
	dpkg -b root gatotray.deb
	
xgatotray: xgatotray.o


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

%.bin32: %.o32
	$(LD) -m32 -o $@ $^

%.o32: %.c %.d
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -m32 -o $@ $<

# Let %.o & %.d depend on %.c included files:
%.d: %.c
	$(CC) -MM $(CPPFLAGS) -MT $*.o -MT $@ -MF $@ $<

# Now include those freshly generated dependencies:
ifneq ($(MAKECMDGOALS),clean)
-include $(depends)
endif
