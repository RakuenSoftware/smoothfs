# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree build wrapper for smoothfs.
#
# Usage:
#   make                     # build against the running kernel headers
#   make KDIR=/path/to/build # build against a specific kernel tree
#   make clean
#   make install             # cp smoothfs.ko into /lib/modules/$(uname -r)/extra
#
# DKMS path is in dkms.conf.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod *.mod.c *.mod.o *.symvers *.order

install: all
	install -D -m 0644 smoothfs.ko \
		$(DESTDIR)/lib/modules/$(shell uname -r)/extra/smoothfs.ko
	depmod -a

.PHONY: all clean install
