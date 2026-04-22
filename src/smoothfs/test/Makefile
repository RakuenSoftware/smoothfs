# SPDX-License-Identifier: GPL-2.0-only
# Build smoothfs userspace test helpers.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
TARGETS  = connectable_fh smb_identity_pin lease_break_agent

all: $(TARGETS)

connectable_fh: connectable_fh.c
	$(CC) $(CFLAGS) -o $@ $<

smb_identity_pin: smb_identity_pin.c
	$(CC) $(CFLAGS) -o $@ $<

lease_break_agent: lease_break_agent.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
