# Makefile for process-monitor.
# Russell Steicke, 2010-04-29.

PREFIX ?= $(HOME)
BIN_PATH = $(PREFIX)/bin
MAN_PATH = $(PREFIX)/share/man/man1

PM       = process-monitor
PROGRAMS = $(PM)
PM_SRCS  = process-monitor.c is_daemon.c log.c envlist.c xmalloc.c

SRCS = $(PM_SRCS)

PM_OBJS  = $(PM_SRCS:.c=.o)
PM_DEPS  = $(PM_SRCS:.c=.d)
DEPS     = $(PM_DEPS)
DEPDEPS = Makefile
PROGRAM_MANS = $(PROGRAMS:=.1)

CFLAGS = -Wall -Werror -g
LDFLAGS = -lutil

# Create the man page from perl POD format.
%.1: %.pod
	pod2man --center="User Commands" --release="User Commands" $< $@

# Create dependency (.d) files from .c files.
%.d: %.c $(DEPDEPS)
	@echo DEP: $<
	@rm -f $@
	@$(CC) -E -M $(CFLAGS) $< > $@


.PHONY: all
all: $(PROGRAMS) $(PROGRAM_MANS)

$(PM) : $(PM_OBJS)

install: all
	install -d $(DESTDIR)$(BIN_PATH) $(DESTDIR)$(MAN_PATH)
	install $(PROGRAMS) $(DESTDIR)$(BIN_PATH)
	install $(PROGRAM_MANS) $(DESTDIR)$(MAN_PATH)

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),distclean)
-include $(DEPS)
endif
endif


.PHONY: clean
clean:
	rm -f $(PROGRAMS) *.o *.d $(PROGRAM_MANS)

.PHONY: distclean
distclean: clean

.PHONY: deb
deb:
	git-buildpackage
