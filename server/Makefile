CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lsqlite3

hollyserver: hollyserver.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: hollyserver
	./hollyserver --selftest
	python3 test_server.py ./hollyserver
	python3 test_accounts.py ./hollyserver

clean:
	rm -f hollyserver

.PHONY: test clean
