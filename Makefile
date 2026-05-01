# SPDX-License-Identifier: GPL-2.0-only

GO ?= go
BASH ?= bash
KDIR ?= /lib/modules/$(shell uname -r)/build
CONTAINER ?= docker
DEBIAN_KERNEL_IMAGE ?= debian:sid
GOFILES := $(shell find . -path ./.git -prune -o -name '*.go' -type f -print)
SHFILES := $(shell find . -path ./.git -prune -o -name '*.sh' -type f -print)

DEBIAN_DKMS_IMAGE ?= debian:trixie

.PHONY: test verify go-test go-vet go-race fmt-check script-check samba-vfs-package-check runtime-harnesses runtime-harnesses-list kernel-build kernel-build-debian dkms-package-debian

test: go-test

verify: fmt-check go-vet go-test go-race script-check samba-vfs-package-check runtime-harnesses-list

go-test:
	$(GO) test ./...

go-vet:
	$(GO) vet ./...

go-race:
	$(GO) test -race ./...

fmt-check:
	@test -z "$$(gofmt -l $(GOFILES) | tee /dev/stderr)"

script-check:
	@for f in $(SHFILES); do $(BASH) -n "$$f"; done

samba-vfs-package-check:
	@if grep -n -E 'x86_64-linux-gnu|aarch64-linux-gnu|/usr/lib/(x86_64|aarch64|arm64)' \
		src/smoothfs/samba-vfs/build.sh \
		src/smoothfs/samba-vfs/debian/rules; then \
		echo "samba-vfs packaging must use Debian multiarch variables, not fixed library paths" >&2; \
		exit 1; \
	fi
	@for arch in x86_64-linux-gnu aarch64-linux-gnu; do \
		output="$$($(MAKE) -s -n -C src/smoothfs/samba-vfs -f debian/rules \
			override_dh_auto_install DEB_HOST_MULTIARCH=$$arch)"; \
		case "$$output" in \
			*"usr/lib/$$arch/samba/vfs/smoothfs.so"*) ;; \
			*) echo "samba-vfs debian/rules does not install to $$arch multiarch path" >&2; \
			   echo "$$output" >&2; \
			   exit 1 ;; \
		esac; \
	done

runtime-harnesses:
	$(BASH) src/smoothfs/test/run_runtime_harnesses.sh

runtime-harnesses-list:
	SMOOTHFS_RUNTIME_DRY_RUN=1 $(BASH) src/smoothfs/test/run_runtime_harnesses.sh

kernel-build:
	$(MAKE) -C src/smoothfs KDIR=$(KDIR)

kernel-build-debian:
	$(CONTAINER) run --rm -v "$(CURDIR):/workspace" -w /workspace $(DEBIAN_KERNEL_IMAGE) \
		bash -euxo pipefail -c '\
			apt-get update; \
			apt-get install -y make gcc bc bison flex libelf-dev libssl-dev "linux-headers-$$(dpkg --print-architecture)"; \
			KDIR="$$(find /lib/modules -maxdepth 2 -type l -name build -print | sort -V | tail -1)"; \
			test -n "$$KDIR"; \
			make kernel-build KDIR="$$KDIR"; \
		'

# Build smoothfs-dkms.deb in a clean debian:trixie container so packaging
# regressions surface in CI rather than only on a fresh appliance install.
# The deb is Architecture: all (DKMS source-only); the host arch only
# determines which lib paths the trixie image uses, so the same target
# runs on amd64 and arm64 hosted runners.
#
# What this proves:
#   - debian/control + debian/rules + debian/smoothfs-dkms.install are
#     consistent with the kernel module's Kbuild source list (#94 caught
#     a missing range_staging.c here).
#   - dpkg-buildpackage runs clean against the trixie samba/dkms toolchain.
#
# What this does NOT prove (yet):
#   - DKMS module compile against the appliance kernel — that needs
#     `apt install ./smoothfs-dkms_*.deb` on a host with linux-headers
#     installed; smoothfs-runtime CI exercises that side via the ops suite.
dkms-package-debian:
	$(CONTAINER) run --rm -v "$(CURDIR):/workspace" -w /workspace $(DEBIAN_DKMS_IMAGE) \
		bash -euxo pipefail -c '\
			apt-get update; \
			apt-get install -y devscripts equivs debhelper dh-dkms; \
			cd src/smoothfs; \
			dpkg-buildpackage -us -uc -b -d; \
			ls -la ../smoothfs-dkms_*_all.deb; \
			dpkg-deb -c ../smoothfs-dkms_*_all.deb | grep -E "range_staging|smoothfs.h|Kbuild|module_signing|kernel_upgrade"; \
		'
