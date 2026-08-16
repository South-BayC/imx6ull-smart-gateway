# SPDX-License-Identifier: GPL-2.0
# Kernel modules orchestrator — dispatches to each driver subdirectory.
# Add new drivers to SUBDIRS as they are created (led, relay, key, ...).

SUBDIRS := led

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d || exit 1; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || exit 1; done

.PHONY: all clean