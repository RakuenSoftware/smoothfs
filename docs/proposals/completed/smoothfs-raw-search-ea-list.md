# smoothfs: `raw.search::ea list` investigation

**Status:** Complete. Resolved in Phase 5.6 and archived as a record of the
triage and root cause. The implemented fix is `smoothfs_listxattr` in
`src/smoothfs/xattr.c`; `raw.search` is now a `MUST_PASS` smbtorture case in
`src/smoothfs/test/smbtorture.sh`.

## Root cause

smoothfs's `inode_operations.listxattr` was wired to `generic_listxattr` (the kernel default). `generic_listxattr` walks `sb->s_xattr` and emits only names from handlers that define a fixed `.name` — it ignores `.prefix`-only passthrough handlers. Our three handlers (`user.` / `trusted.` / `security.`) are all prefix-only, so `generic_listxattr` returned an empty list for every file regardless of what xattrs actually existed on the lower.

Samba's `SMB_VFS_FLISTXATTR` path (used by `get_ea_names_from_fsp`, which feeds `SMB_FIND_FILE_EA_LIST`) calls `flistxattr(2)` which goes through `inode_operations.listxattr`. Empty list → no EAs in the SEARCH response → the test's `result.list[1].ea_list.eas.eas[0].value.length` comes out as 0.

`getxattr`-by-name still worked because the prefix handlers' `.get` correctly forwarded to the lower; only enumeration was broken. That's why earlier straight `getfattr -n user.X` succeeded in tests and the defect slipped past every `smbtorture` suite that didn't enumerate EAs explicitly.

## Fix

Add `smoothfs_listxattr` — delegates to `vfs_listxattr` on the lower dentry — and wire it into all four smoothfs `inode_operations` variants instead of `generic_listxattr`. One call site, one line change per site.

## Verification

- Manual: `getfattr -d` via smoothfs now returns the user.* names set through either path (direct setfattr via smoothfs, or SMB setxattr via Samba).
- `smbtorture raw.search.ea list` — PASS.
- `smbtorture raw.search` — PASS (all subtests).
- `src/smoothfs/test/smbtorture.sh` — `raw.search` promoted from `KNOWN_ISSUES` to `MUST_PASS` (now 16 tests).
- `src/smoothfs/test/smbtorture_xfs_baseline.sh` — KNOWN_ISSUES list updated to the remaining four Samba/Linux-level failures; `raw.search` no longer runs against XFS as a baseline comparison, because there is no asymmetry to compare any more.

## Historical Failure

`smbtorture //.../share raw.search` — specifically its `ea list` subtest — fails when the share is a smoothfs mount but passes against a plain XFS mount with the same `smb.conf`:

```
source4/torture/raw/search.c:1551: (result.list[1].ea_list.eas.eas[0].value.length)
    was 0 (0x0), expected 9 (0x9): incorrect value
```

The test creates a directory with two files, sets a user xattr with a 9-byte value on each, issues a `RAW_SEARCH_EA_LIST` (`SMB_FIND_FILE_EA_LIST`, level 221) against the directory, and checks that the server response contains each file's EA with the right value length. Against smoothfs the name comes back but the value length is 0 for at least one of the entries.

Confirmed smoothfs-specific by `src/smoothfs/test/smbtorture_xfs_baseline.sh` (Phase 5.5): the same test against plain XFS, same Samba config and port, passes.

## Historical Triage

**Basic xattr round-trip is fine.** On a smoothfs mount, `setfattr -n user.alpha -v hello` on a file followed by `getfattr -d` returns the value correctly. `strace` confirms `listxattr` reports both names and their lengths, and `getxattr` returns the right bytes for each. So the regression isn't in `smoothfs_xattr_handlers` itself — user.* passthrough to the lower works on the happy-path syscall surface.

**It's not a pure listxattr coverage bug.** Earlier hypothesis: smoothfs's `inode_operations.listxattr = generic_listxattr` only enumerates known xattr handler names and misses user.* entries on the lower. Ruled out by strace above.

**It's not the same failure as the `raw.rename / smb2.rw / smb2.getinfo / smb2.setinfo` cluster.** Those four fail on plain XFS for Samba/Linux reasons orthogonal to smoothfs. `raw.search::ea list` is the only one of the five where smoothfs is the regression surface.

## Historical Defect Hypothesis

Samba serves `RAW_SEARCH_EA_LIST` via its readdir-plus-EAs fast path: for each directory entry returned by `SMB_VFS_READDIR`, it reads a caller-specified list of EAs and packs them into the reply without opening each file via the normal path-walk. That fast path differs from the `getfattr` surface in one or more of:

- Whether EAs are read via an O_PATH fd or a name lookup — stacked filesystems sometimes handle these differently when `d_fsdata` isn't primed on a fresh anonymous dentry.
- Whether the `dentry` Samba supplies is the one smoothfs produced from `smoothfs_lookup`, or a reconstructed dentry from the readdir stream.
- Subtle interaction with `trusted.smoothfs.*` reserved xattrs — the fast path may list-then-get in a single trip, and smoothfs's `trusted_xattr_get` serves `smoothfs.fileid` / `smoothfs.lease` from `si` directly. If Samba is looking up an EA *name* that overlaps with one of those, the mismatched storage surface could manifest as a zero-length value.

The fix will almost certainly be a targeted change in `src/smoothfs/xattr.c` or `src/smoothfs/inode.c` (likely adding an `->listxattr` inode op that calls the lower's `vfs_listxattr` directly, or tightening the dentry → lower-dentry resolution in the xattr handlers so the fast path sees the same state as the slow path).

## Historical Next Steps

1. Reproduce the failure with full `dmesg` tracing enabled around the smoothfs xattr handlers, triggered specifically by `raw.search::ea list` — observe which EA name returns 0-length and on which file.
2. If tracing confirms the fast-path hypothesis, add `->listxattr` (inode op) that delegates to the lower, plus a `->getxattr`-under-lock path that re-primes `d_fsdata` when needed.
3. Promote `raw.search` from `KNOWN_ISSUES` into `MUST_PASS` in `src/smoothfs/test/smbtorture.sh` once the fix lands.

## Historical Deferral Rationale

The visibility of the bug is low (affects only servers that expose smoothfs shares via SMB and only one subtest of one `smbtorture` suite), the fix needs a real tracing pass to choose between the two candidate root causes, and Phase 5.5's primary goal — correcting the Phase 5.4 triage so future work doesn't chase non-bugs — is achieved by the baseline harness + the `KNOWN_ISSUES` comment restructure alone. Landing a speculative fix without a confirmed root cause would be worse than documenting the gap.
