# For multiple programs using a single source file each,
# we can just define 'progs' and create custom targets.
PROGS	=	bridge bridge-b bridge-p
LIBNETMAP =

CLEANFILES = $(PROGS) *.o

SRCDIR ?= ../..
VPATH = $(SRCDIR)/apps/bridge

NO_MAN=
CFLAGS = -O2 -pipe
CFLAGS += -Werror -Wall -Wunused-function
CFLAGS += -I $(SRCDIR)/sys -I $(SRCDIR)/apps/include
CFLAGS += -Wextra

LDLIBS += -lpthread
ifeq ($(shell uname),Linux)
	LDLIBS += -lrt	# on linux
endif

PREFIX ?= /usr/local

all: $(PROGS)

clean:
	-@rm -rf $(CLEANFILES)

.PHONY: install
install: $(PROGS:%=install-%)

install-%:
	install -D $* $(DESTDIR)/$(PREFIX)/bin/$*

bridge: bridge.o
bridge.o: bridge.c
	$(CC) $(CFLAGS) -c $^ -o $@

bridge-b: bridge-b.o
bridge-b.o: bridge.c
	$(CC) $(CFLAGS) -DBUSYWAIT -c $^ -o $@

bridge-p: bridge-p.o
bridge-p.o: bridge.c
	$(CC) $(CFLAGS) -DPB -c $^ -o $@
