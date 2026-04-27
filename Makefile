# SPDX-License-Identifier: GPL-2.0-only

GO ?= go

.PHONY: test go-test

test: go-test

go-test:
	$(GO) test ./...
