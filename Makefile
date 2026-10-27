# simpled - a simple init that reads systemd units
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

BIN = simpled simplectl
SRC = simpled.c simplectl.c unit.c util.c
OBJ = $(SRC:.c=.o)
UNITS = units/sysinit.target units/multi-user.target units/emergency.service
LINKS = poweroff reboot halt

all: $(BIN)

config.h:
	cp config.def.h $@

$(OBJ): config.h config.mk unit.h util.h

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

simpled: simpled.o unit.o util.o
	$(CC) -o $@ simpled.o unit.o util.o $(LDFLAGS)

simplectl: simplectl.o unit.o util.o
	$(CC) -o $@ simplectl.o unit.o util.o $(LDFLAGS)

check: all
	./tests/run.sh

clean:
	rm -f $(BIN) $(OBJ)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/simpled/system
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin
	cd $(DESTDIR)$(PREFIX)/bin && chmod 755 $(BIN)
	for l in $(LINKS); do ln -sf simplectl $(DESTDIR)$(PREFIX)/bin/$$l; done
	cp -f $(UNITS) $(DESTDIR)$(PREFIX)/lib/simpled/system

uninstall:
	cd $(DESTDIR)$(PREFIX)/bin && rm -f $(BIN) $(LINKS)
	cd $(DESTDIR)$(PREFIX)/lib/simpled/system && rm -f $(UNITS:units/%=%)

.PHONY: all check clean install uninstall
