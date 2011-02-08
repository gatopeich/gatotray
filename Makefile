### Generic Makefile for C code
#
#  (c) 2011, gatopeich, licensed under a Creative Commons Attribution 3.0
#  Unported License: http://creativecommons.org/licenses/by/3.0/
#  Briefly: Use it however suits you better and just give me due credit.
#
### Changelog:
# V2.1: Added CCby license. Restructured a bit.
# V2.0: Added 32-bit target for 64 bits environment.

CFLAGS := -std=c99 -Wall -O3 $(CFLAGS)
CPPFLAGS := `pkg-config --cflags gtk+-2.0` $(CPPFLAGS)
LDFLAGS := `pkg-config --libs gtk+-2.0` $(LDFLAGS)
CC := gcc
LD := gcc $(LDFLAGS)

### Specific targets
targets := gatotray

all: $(targets)

gatotray: gatotray.o
	$(LD) -o $@ $^

gatotray.bin32: gatotray.o32
	$(LD) -m32 -o $@ $^

install: gatotray
	strip $^
	install $^ /usr/local/bin
	install gatotray.xpm /usr/share/icons


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

%.o32: %.c %.d
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -m32 -o $@ $<

# Let %.o & %.d depend on %.c included files:
%.d: %.c
	$(CC) -MM $(CPPFLAGS) -MT $*.o -MT $@ -MF $@ $<

# Now include those freshly generated dependencies:
ifneq ($(MAKECMDGOALS),clean)
-include $(depends)
endif
