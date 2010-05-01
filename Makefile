# Makefile for child-monitor.
# Russell Steicke, 2010-04-29.

PROGRAM = child-monitor
SRCS = child-monitor.c is_daemon.c log.c envlist.c

OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)
DEPDEPS = Makefile
PROGRAM_MAN = $(PROGRAM).1

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
all: $(PROGRAM) $(PROGRAM_MAN)

$(PROGRAM): $(OBJS)

install: all
	cp $(PROGRAM) /usr/local/bin/
	cp $(PROGRAM_MAN) /usr/local/share/man/man1/

ifneq ($(MAKECMDGOALS),clean)
-include $(DEPS)
endif


.PHONY: clean
clean:
	rm -f $(PROGRAM) *.o *.d $(PROGRAM_MAN)

