#!/bin/bash
# Real-kernel generic-netlink receive cancellation smoke test.
#
# Service.Run relies on Client.Close interrupting a goroutine blocked in
# Client.Receive. Unit tests cover that with a fake client; this harness covers
# the actual smoothfs generic-netlink socket against a loaded kernel module.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
ROOT=${ROOT:-/tmp/smoothfs-netlink-receive-close}

cleanup() {
	rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$ROOT"

HELPER="$ROOT/netlink_receive_close.go"
cat > "$HELPER" <<'GO'
package main

import (
	"fmt"
	"os"
	"time"

	smoothfs "github.com/RakuenSoftware/smoothfs"
)

func main() {
	client, err := smoothfs.Open()
	if err != nil {
		fmt.Fprintf(os.Stderr, "open smoothfs client: %v\n", err)
		os.Exit(1)
	}

	started := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		close(started)
		_, err := client.Receive()
		done <- err
	}()
	<-started

	/* Give the goroutine a chance to enter the blocking netlink receive
	 * syscall. If Close does not interrupt that syscall, the timeout below
	 * catches it. */
	time.Sleep(200 * time.Millisecond)

	if err := client.Close(); err != nil {
		fmt.Fprintf(os.Stderr, "close smoothfs client: %v\n", err)
		os.Exit(1)
	}

	select {
	case err := <-done:
		if err == nil {
			fmt.Fprintln(os.Stderr, "receive returned nil after close; want an interrupted receive error")
			os.Exit(1)
		}
		fmt.Printf("receive returned after close: %v\n", err)
	case <-time.After(5 * time.Second):
		fmt.Fprintln(os.Stderr, "receive did not return within 5s after client close")
		os.Exit(1)
	}
}
GO

echo "=== netlink Receive returns after Client.Close ==="
if (cd "$REPO_ROOT" && timeout 10s go run "$HELPER"); then
	echo "netlink_receive_close: PASS"
else
	echo "netlink_receive_close: FAIL"
	exit 1
fi
