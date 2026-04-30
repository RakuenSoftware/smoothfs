# SPDX-License-Identifier: GPL-2.0-only

GO ?= go
BASH ?= bash
KDIR ?= /lib/modules/$(shell uname -r)/build
GOFILES := $(shell find . -path ./.git -prune -o -name '*.go' -type f -print)
SHFILES := $(shell find . -path ./.git -prune -o -name '*.sh' -type f -print)

.PHONY: test verify go-test go-vet go-race fmt-check script-check samba-vfs-package-check kernel-build

test: go-test

verify: fmt-check go-vet go-test go-race script-check samba-vfs-package-check

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

kernel-build:
	$(MAKE) -C src/smoothfs KDIR=$(KDIR)
