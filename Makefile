
CFLAGS = -Wall -Werror -g
LDFLAGS = -lutil
PROG = child-monitor
PROG_OBJ = $(PROG).o is_daemon.o log.o envlist.o
PROG_MAN = $(PROG).1

%.1: %.pod
	pod2man \
		--center="User Commands" \
		--release="User Commands" \
		$< $@

.PHONY: default
default: $(PROG) $(PROG_MAN)

$(PROG): $(PROG_OBJ)

install: default
	cp $(PROG) /usr/local/bin/
	cp $(PROG_MAN) /usr/local/share/man/man1/

.PHONY: clean
clean:
	rm -f $(PROG) $(PROG_OBJ) $(PROG_MAN)

