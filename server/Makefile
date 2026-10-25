CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra

hollyserver: hollyserver.c
	$(CC) $(CFLAGS) -o $@ $<

test: hollyserver
	python3 test_server.py ./hollyserver

clean:
	rm -f hollyserver

.PHONY: test clean
