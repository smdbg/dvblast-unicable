CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
PREFIX ?= /usr/local
PYTHON ?= python3

all: unicable-tune dvb-scan lock-adapter7

unicable-tune: src/unicable-tune.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

dvb-scan: src/dvb-scan.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

# Compatibility with scripts using the original helper name.
lock-adapter7: unicable-tune
	ln -sf unicable-tune $@

# Tests are synthetic or --dry-run and never open DVB hardware.
check: all
	$(PYTHON) tests/test-unicable-tune.py ./unicable-tune
	$(PYTHON) tests/test-dvb-scan.py ./dvb-scan
	@set -eu; test_dir=$$(mktemp -d); trap 'rm -rf "$$test_dir"' EXIT; \
		$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test-lock-status.c $(LDLIBS) -o "$$test_dir/status"; \
		"$$test_dir/status"

install: all
	install -D -m 0755 unicable-tune "$(DESTDIR)$(PREFIX)/bin/unicable-tune"
	install -D -m 0755 dvb-scan "$(DESTDIR)$(PREFIX)/bin/dvb-scan"
	ln -sf unicable-tune "$(DESTDIR)$(PREFIX)/bin/lock-adapter7"

clean:
	rm -f unicable-tune dvb-scan lock-adapter7

.PHONY: all check install clean
