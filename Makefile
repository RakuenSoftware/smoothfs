# SPDX-License-Identifier: GPL-2.0-only

GO ?= go
BASH ?= bash
KDIR ?= /lib/modules/$(shell uname -r)/build
GOFILES := $(shell find . -path ./.git -prune -o -name '*.go' -type f -print)
SHFILES := $(shell find . -path ./.git -prune -o -name '*.sh' -type f -print)

.PHONY: test verify go-test go-vet go-race fmt-check script-check kernel-build

test: go-test

verify: fmt-check go-vet go-test go-race script-check

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

kernel-build:
	$(MAKE) -C src/smoothfs KDIR=$(KDIR)
