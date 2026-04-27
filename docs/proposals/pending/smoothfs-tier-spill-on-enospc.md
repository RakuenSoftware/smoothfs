# Proposal: smoothfs — Tier Spill on Near-ENOSPC

**Status:** Done. Implementation has fully landed:
- `smoothfs_create` and `smoothfs_mkdir` in `src/smoothfs/inode.c` walk
  the tier list on `-ENOSPC` (see `inode.c:697..794` for create and
  `inode.c:984..1079` for mkdir), call `smoothfs_spill_note_success`
  on the first tier with room, and `smoothfs_spill_note_failed_all_tiers`
  if the whole pool refuses.
- `smoothfs_ensure_dir_path_on_tier` (§5.1) materialises parent chains
  on the spill tier before the child create.
- Per-pool sysfs counters `spill_creates_total`,
  `spill_creates_failed_all_tiers`, and `any_spill_since_mount` are
  exported alongside the rest of the pool sysfs bundle in `super.c`.
- `SMOOTHFS_CMD_SPILL` netlink event is emitted on every successful
  spill (see `netlink.c`).
- Union readdir landed: spilled files appear in `readdir` on the
  smoothfs mount (`tier_spill_union_readdir.sh` asserts this).
- All §11 tests are present: `tier_spill_basic_create.sh`,
  `tier_spill_nested_parent.sh`, `tier_spill_rename_xdev.sh`,
  `tier_spill_unlink_finds_right_tier.sh`,
  `tier_spill_union_readdir.sh`, and `tier_spill_crash_replay.sh`.
- The §11.7 SmoothNAS-side `--delete` guard against spill-active
  destinations is implemented in tierd at
  `tierd/internal/api/backup.go:661` with regression coverage in
  `backup_delete_guard_test.go`.

This document is kept in `pending/` as historical record matching the
project convention used by `smoothfs-samba-vfs-module.md`.

**Parent:** [`smoothfs-stacked-tiering.md`](./smoothfs-stacked-tiering.md)
**Depends on:** Phase 1 (inode passthrough), Phase 2 (movement cutover). No
new dependency on Phase 5 (xattr) beyond what is already live.

---

## 1. Problem

`smoothfs_create` and `smoothfs_mkdir` always create the lower object on
`SMOOTHFS_I(dir)->lower_path.mnt` — the mount of whichever tier the
parent directory happens to sit on. For directories originating at the
smoothfs mount root, that is always `sbi->tiers[sbi->fastest_tier]`
(tier 0).

When tier 0 fills (the lower filesystem returns `-ENOSPC`), the
`->create` call fails and the error is propagated unchanged to
userspace. On a SmoothNAS appliance with a fast-but-small NVMe tier 0
(64 GB — 2 TB is typical) and a slower-but-large ZFS or HDD tier 1
(multi-TB), this causes any single backup larger than tier 0 to abort
mid-write even when the pool as a whole has ample room. Observed on
the SmoothNAS backup path against `192.168.1.254:/mnt/data/storage`:
a `/backup/` subtree of 329 GB fails at the 64 GB mark of tier 0 with
`rsync: destination filesystem is full`, despite tier 1 being at 1 %
used.

This proposal pins the semantics for admission fallback from tier _k_
to tier _k+1_ on `-ENOSPC` or near-ENOSPC, and the matching lookup /
readdir / rename / unlink / rmdir semantics needed to keep the filesystem
consistent once files start landing on non-fastest tiers at create time.

## 2. Non-goals

- **Movement-driven promotion / demotion.** Movement already exists
  (`smoothfs_movement.c` + `MOVE_PLAN` / `MOVE_CUTOVER`). Nothing in
  this proposal changes movement. Spill lets a file _start_ on a
  non-fastest tier; movement lets it _change_ tier later. The two
  mechanisms touch the same `lower_path` swap primitive but are
  ordered differently.
- **Write-time re-admission.** If an `open(O_WRONLY)` exists on a
  file that was created on tier 0 and the next `write()` hits ENOSPC
  on tier 0 mid-file, this proposal does not migrate the file to
  tier 1 under the existing fd. That case becomes `-ENOSPC` to the
  caller exactly as today. Movement-driven eviction under write
  pressure is a separate Phase 3+ proposal.
- **Balancing.** Creates still prefer the fastest tier with room.
  This proposal does not stripe, round-robin, or weight across tiers.
- **Phase 2 union readdir.** See §8 for the interaction; a proper
  union readdir is its own proposal.

## 3. On-disk model invariants this proposal adds

### 3.1 A smoothfs file may physically live on any tier at any time

Today the implicit rule is "new files live on tier 0 until movement
moves them." After this proposal the rule is **"a smoothfs file lives
on whichever tier its inode's `lower_path.mnt` points at, period."**
That already matched the code for post-movement state
(`movement.c:234` swaps `si->lower_path` on cutover). This proposal
makes it true for create state as well.

### 3.2 A smoothfs directory may exist on multiple tiers simultaneously

As soon as tier 0 `/a/b/` is full and a new file needs to be created
under `/a/b/` on tier 1, the lower filesystem on tier 1 must have a
directory `/a/b/` into which that file is placed. smoothfs maintains
this by materialising the parent-directory chain on any tier a spill
lands on. Both copies of `/a/b/` are independent lower directories
owned by their tier's backing filesystem; smoothfs picks one of them
as the "canonical" `lower_path` for the smoothfs directory inode and
uses the other(s) only as lookup fallback targets (see §4.2.)

The canonical tier for a smoothfs directory is the tier of the
_first_ materialisation. If `/a/b/` first materialised on tier 0, it
stays canonical on tier 0 even after tier 1 also grows an `/a/b/`.

### 3.3 Per-tier orphans are tolerated; per-tier duplicates are not

If the same relative path exists as a regular file on two tiers at
mount time, mount fails with `-EEXIST` logged via `pr_err`. This is
already the state replay machinery's implicit assumption (it picks
the tier with the placement-log record). Enforcing it explicitly is
part of this proposal because spill creates a new path by which
duplicates can arise (race between two mounts creating the same file
on different tiers).

Orphans (file on tier 1 with no placement-log record) are picked up
by the existing `smoothfs_placement_replay` scan. This proposal
makes orphans from spill the normal case, not the exception.

## 4. Operation semantics

### 4.1 `smoothfs_create`

Given a positive smoothfs parent dir and a negative smoothfs dentry:

1. Let `p = SMOOTHFS_I(dir)->current_tier`. Attempt `->create` on
   `sbi->tiers[p]`'s lower as today.
2. If the attempt returns anything other than `-ENOSPC`, propagate
   as today (including success).
3. If `-ENOSPC`: for each `t in {p+1, p+2, …, sbi->ntiers - 1}`:
   a. Compute `rel_path` = dentry-path-raw of `dir` relative to the
      smoothfs mount root. Cached in `SMOOTHFS_I(dir)->rel_path`
      when non-NULL; otherwise computed and not cached (spill is
      rare enough that we don't pay cache cost in steady state).
   b. Call `smoothfs_ensure_dir_path_on_tier(sbi, t, rel_path,
      &lower_parent_t)`. This mkdir-p's each missing component of
      `rel_path` on tier `t`, returning a held `struct path` to the
      final directory on tier `t`. See §5.1.
   c. Take `inode_lock` on `lower_parent_t.dentry`'s inode.
   d. `lookup_one_len(dentry->d_name, lower_parent_t.dentry, …)` to
      produce a negative `lower_t`. (Positive here → duplicate →
      `-EEXIST` out.)
   e. `smoothfs_compat_create(idmap, lower_parent_t_inode, lower_t,
      mode, excl)`.
   f. Unlock `lower_parent_t`.
   g. If success: `inode = smoothfs_iget(sb, &path_on_tier_t,
      false)`, `SMOOTHFS_I(inode)->current_tier = t`,
      `d_instantiate`, emit a `SMOOTHFS_MS_PLACED` placement record
      with `current_tier = t`, return 0.
   h. If `-ENOSPC`: `dput(lower_t)`, continue.
   i. Any other error: propagate.
4. If every tier returned `-ENOSPC`: the whole pool is full.
   Return `-ENOSPC` with the original (tier `p`) error, which
   matches existing userspace expectations.

Crash safety: the placement record must be fsynced before the file
name becomes visible via smoothfs. Acceptable weakening: the
placement record is written `sync=false` (same as `MS_PLACED` from
movement cutover at `placement.c:439`). Worst case post-crash is
that on mount the placement-replay scan finds the orphan on tier
`t`, reads its `trusted.smoothfs.oid` xattr, and re-records it.
If the xattr was not yet set when we crashed, see §4.7.

### 4.2 `smoothfs_lookup`

Positive parent, negative smoothfs dentry, name to resolve:

1. Look up on parent's tier exactly as today
   (`smoothfs_compat_lookup` on `SMOOTHFS_I(dir)->lower_path`).
2. If **positive**, use it. Done.
3. If **negative**: fall back to the existing replay-record path
   first (`smoothfs_lookup_rel_path`) as today. This handles
   cutover-moved files whose replay records remain resident in
   memory, and crash-recovered spills whose placement record was
   replayed at mount.
4. If the replay path also finds nothing: for each `t != p`:
   a. Resolve `rel_path` on tier `t` via
      `smoothfs_resolve_rel_path(&sbi->tiers[t], rel_path, &p_t)`.
   b. Lookup `name` in `p_t.dentry`. If positive, that's our
      spilled file. `smoothfs_iget` against tier `t`, set
      `current_tier = t`, return. Before returning, emit a
      `SMOOTHFS_MS_PLACED` placement record for the oid so future
      lookups go through the fast path; see §4.7 for oid handling.
   c. If negative on tier `t`, continue to tier `t+1`.
5. If no tier produced a hit: return negative as today.

**Perf note.** The tier walk is `O(ntiers)` `vfs_path_lookup`s per
negative-on-parent lookup. For `ntiers = 2` (the common case) this
is one extra lookup. Negative lookups on files that truly don't
exist pay the full walk. We accept this on the grounds that
negative-lookup workloads are dominated by `open(O_CREAT)` paths
that follow up with a create — and the subsequent create doesn't
care whether the lookup walked one tier or three.

### 4.3 `smoothfs_mkdir`

Analogous to §4.1. Dirs take `O(1)` blocks so `-ENOSPC` on mkdir is
rare but not impossible (inode exhaustion, quota, XFS log
pressure). Same spill cascade applies. A mkdir that lands on a
non-parent tier creates one more directory on that tier; parent of
that dir is the `lower_parent_t` materialised by
`smoothfs_ensure_dir_path_on_tier`.

### 4.4 `smoothfs_rmdir`

No change. `smoothfs_lower_dentry(dentry)` already points at the
tier the directory actually lives on (`current_tier`), so
`vfs_rmdir` on that lower parent works as today.

Caveat: if the directory exists on multiple tiers (§3.2), this
removes only the canonical-tier copy. The non-canonical copies on
other tiers become tier-local orphans until movement cleans them
up. Not a correctness bug (files on those tiers are still
addressable via §4.2 lookup fallback), but worth documenting.

Phase 3+ follow-up: after a successful canonical rmdir, scan other
tiers for matching-path empty dirs and rmdir them too. Deferred —
not on the hot path.

### 4.5 `smoothfs_unlink`

No change. Same reasoning as rmdir: lookup already resolved to the
right tier, unlink on that tier's lower works.

If the smoothfs inode's `current_tier` was set by §4.2 fallback
lookup (i.e. a spilled file), `unlink` updates the placement
record to `SMOOTHFS_MS_CLEANUP_COMPLETE` as today — no new code
path needed. Placement replay treats cleanup-complete records as
terminal.

### 4.6 `smoothfs_rename`

Allowed when `src->current_tier == dst_dir->current_tier` (both
operate on the same lower filesystem). Cross-tier rename returns
`-EXDEV` — userspace (rsync, `mv`) will fall back to copy+unlink,
which goes through `smoothfs_create` and picks up spill semantics
naturally.

The cross-tier case could in principle be supported by combining
cutover semantics with rename. Not in this proposal; it's a small
additional Phase-3 delta if a workload ever needs it.

### 4.7 Object IDs

Spilled files need a `trusted.smoothfs.oid` xattr set on the lower
before the placement record is emitted, otherwise placement replay
cannot re-associate the orphan with a smoothfs inode. Existing
movement code (`movement.c:115`) already sets the xattr during
cutover; this proposal reuses that helper (renamed from
`smoothfs_cutover_set_oid` → `smoothfs_set_oid_xattr` if needed)
at step 4.1.g before the placement record.

If the xattr-set fails (ENOSPC on the lower's xattr area — rare
but possible on XFS at 100 % full), we unlink the spilled file and
propagate the error. No partial success.

## 5. New helpers

### 5.1 `smoothfs_ensure_dir_path_on_tier`

```c
int smoothfs_ensure_dir_path_on_tier(struct smoothfs_sb_info *sbi,
                                     u8 tier,
                                     const char *rel_path,
                                     struct path *out_parent);
```

Behaviour:

- `rel_path` is tier-relative (`""` means the tier's root); components
  are slash-separated. `NULL` or `""` returns `sbi->tiers[tier]
  .lower_path` with an extra `path_get`.
- Walks components left-to-right under
  `sbi->tiers[tier].lower_path.dentry`. For each component:
  - `smoothfs_compat_lookup`. If positive dir, descend.
  - If positive non-dir → `-ENOTDIR` (tier corruption, refuse).
  - If negative → `smoothfs_compat_mkdir` with
    `S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH` (0755).
    Attrs intentionally default; rsync will `setattr` parent dirs
    on its own traversal if it cares.
- Returns `*out_parent` as a held `struct path` (caller must
  `path_put` when done) pointing at the final directory on this
  tier.
- On failure: partial creates are intentionally **not** rolled back.
  A half-built parent chain becomes an empty orphan directory tree;
  it is harmless (no data in it), takes ~O(depth) inodes, and
  will be cleaned up by a future Phase 3 scrubber or by the next
  successful spill filling those dirs with content.

Concurrency: any two threads racing to spill the same file under
the same parent path can race on `smoothfs_compat_mkdir`; one gets
`-EEXIST` which is treated as success ("someone else created it,
proceed"). The final child-file `->create` in §4.1.e is serialised
by `lower_parent_t`'s inode lock.

### 5.2 `smoothfs_lookup_rel_across_tiers`

Internal helper invoked by §4.2 step 4. Signature:

```c
int smoothfs_lookup_rel_across_tiers(struct smoothfs_sb_info *sbi,
                                     u8 exclude_tier,
                                     const char *rel_path,
                                     struct path *out);
```

Walks tiers in rank order skipping `exclude_tier`. Returns first
positive result or `-ENOENT`.

## 6. Placement-log contract deltas

This proposal does not add new placement log record types. Records
emitted on spill use existing states:

- **On spill create (§4.1.g):** one `SMOOTHFS_MS_PLACED` record,
  `current_tier = t`, `intended_tier = t`.
- **On spill discovery via §4.2 lookup fallback:** one
  `SMOOTHFS_MS_PLACED` record so the next lookup goes through
  `smoothfs_lookup_rel_path` rather than the tier walk.
- **On unlink/rmdir of a spilled object:** one
  `SMOOTHFS_MS_CLEANUP_COMPLETE` as today.

Placement replay (§0.4 of `smoothfs-phase-0-contract.md`) continues
to be authoritative: at mount, any orphan scanned off a lower
filesystem is associated with its oid-xattr and creates an in-memory
replay record. Spill creates do not change the set of replay
scenarios; they just make them common in steady state.

## 7. Interaction with the movement planner

tierd's planner (the userspace side) reads `MOVE_PLAN` candidates
from its heat tracker and issues `MOVE_PLAN` netlink ops to the
kernel. After this proposal:

- A freshly-spilled file on tier 1 is indistinguishable from a file
  that moved to tier 1 via cutover. The planner sees it as a tier-1
  object with heat tracked normally.
- The planner MAY choose to `MOVE_PLAN(tier=0)` on a spilled object
  once tier 0 has room again. Desirable default.
- The planner MUST NOT `MOVE_PLAN(tier=0)` on a spilled object
  while tier 0 is still at or above its high-water mark. That logic
  exists on the userspace side and is out of scope here.

No kernel-side change to netlink/UAPI.

## 8. Readdir interaction

`smoothfs_iterate_shared` today delegates to the canonical-tier
lower's `iterate_shared`. Spilled files on non-canonical tiers are
**invisible to `readdir`** until:

- (a) the planner moves them onto the canonical tier via cutover, or
- (b) a proper Phase 2 union readdir lands (not in this proposal).

**Consequences operators must be aware of:**

- Fresh backups (empty destination) complete correctly; all files
  are reachable by name so rsync's source-driven traversal lands
  every file in the right place. The destination `readdir` is
  never consulted during a fresh backup.
- **Incremental `rsync` with `--delete` on a destination that has
  spilled files will see the spilled files as "missing" and
  reconstruct them.** In the best case this re-transfers
  already-present files (wasted bandwidth, correct result).
  In the worst case `--delete` is paired with a broken source
  enumeration and the destination loses data that's on a non-canonical
  tier. The tierd `backup` package MUST NOT combine
  `--delete` with spill-prone workloads until Phase 2 union readdir
  lands. Enforcement: `backup.go` adds a guard that refuses
  `--delete` (or `-delete-after`) when the destination pool has
  `spill_active = true`. The `spill_active` flag is reported by
  smoothfs via netlink (new counter `any_spill_since_mount`; see
  §12).
- `ls /mnt/pool/` listings are incomplete for spilled files.
  Operators should be told up front via the support matrix and
  the Samba VFS layer's `readdir` behaviour documentation.

The readdir limitation is the single biggest reason this proposal
is "spill phase 1", not "spill + union readdir". Phase 2 union
readdir is its own multi-week effort (cookie reconciliation across
tiers, per-tier d_off rewriting, dedup on canonical-wins rules) and
must not block the ENOSPC fix.

## 9. Crash safety and recovery

Five crash points to reason about:

1. **Crash between the tier-`t` `->create` and the oid xattr set.**
   Orphan file on tier `t` with no oid. `smoothfs_placement_replay`'s
   tree scan encounters it, sees no oid xattr, synthesises one, and
   creates a replay record. This is the same recovery path used for
   legacy pre-smoothfs files in `smoothfs-phase-0-contract.md`
   §0.4.4. No special case needed.
2. **Crash between oid set and placement record write.** Orphan
   file with a valid oid. Replay tree scan finds it and emits the
   record. Same as (1) for correctness; slightly faster because
   the oid is already set.
3. **Crash mid-`smoothfs_ensure_dir_path_on_tier`.** Partial dir
   chain on tier `t`. No oid, no file, no placement record.
   Empty dir chain is harmless (§5.1 disclaimer). A retry of the
   same rel_path succeeds and fills it in.
4. **Crash after placement record is written but before
   `d_instantiate`.** User-visible effect is zero: the dentry was
   never cached. On next mount the placement replay picks up the
   file and surfaces it via `smoothfs_lookup_rel_path`.
5. **Crash during `smoothfs_lookup_rel_across_tiers` (§4.2 step 4,
   the "discover-and-record" variant).** No mutations to the
   lower tree; only a placement record emit was contemplated.
   If the placement record emit hadn't happened yet, the next
   lookup does the tier walk again. Idempotent.

No new crash-recovery code paths beyond what placement replay
already does.

## 10. Security / permissions

`smoothfs_ensure_dir_path_on_tier` creates intermediate directories
with a hardcoded 0755. This differs from parent-directory-mode on
tier 0, which the Phase-0 contract doesn't forbid but is a change
operators might notice. Acceptable because:

- Users cannot observe the per-tier directory mode directly
  (smoothfs's dir mode comes from the canonical-tier dir).
- rsync's `--perms` / `-a` flags set the correct mode on the
  canonical copy; the non-canonical duplicate dir stays at 0755
  and is never user-facing.
- Future Phase-3 cleanup can propagate mode from the canonical
  tier when materialising.

If the operator requires stricter defaults, the intermediate-dir
mode can be made a mount option (`spill_mkdir_mode=0700`). Not in
this proposal.

## 11. Tests (MUST PASS before merge)

Under `src/smoothfs/test/`:

1. **`tier_spill/basic_create.sh`** — create a 2-tier smoothfs pool,
   fill tier 0 to 98 % with a single large file, create a second
   large file; second file must succeed, must appear on tier 1 per
   `/sys/class/bdi/` accounting, must be readable end-to-end with
   correct sha256, must appear in `smoothfs_lookup` by name.
2. **`tier_spill/nested_parent.sh`** — same but the second file
   lives at `/a/b/c/d.bin`. Verify `/a`, `/b`, `/c` exist on tier 1
   as directories after the spill.
3. **`tier_spill/rename_xdev.sh`** — after a spill, attempt a
   cross-tier rename; expect `-EXDEV`. `rsync --remove-source-files`
   must continue via copy+unlink.
4. **`tier_spill/unlink_finds_right_tier.sh`** — after a spill,
   `unlink` the spilled file; verify tier 1 backing is cleaned up
   and placement record becomes `CLEANUP_COMPLETE`.
5. **`tier_spill/readdir_limitation.sh`** — after a spill, confirm
   that `readdir` does NOT show spilled files and the test
   explicitly asserts this so the limitation can't silently
   disappear. The same test confirms `ls` by name still works
   (§4.2 lookup). When Phase 2 union readdir lands, this test
   flips its assertion.
6. **`tier_spill/crash_replay.sh`** — run an fsync-interposed
   create on tier 1, kill the kernel module, reload, remount;
   verify the spilled file is re-discovered by placement replay.
7. **`tier_spill/no_delete_during_spill.sh`** — live-level integration
   test against `backup.go`: attempt an `rsync --delete` against a
   spill-active destination; expect the tierd backup handler to
   refuse with an explicit error pointing at this proposal's §8.

Existing Phase-1 tests (create/mkdir/unlink/rename) must continue
to pass with spill enabled but never triggered (tier 0 has ample
room). No regression.

## 12. Observability

Two new kernel-side counters exported via the existing `sysfs`
bundle used by the pool root (see `super.c:smoothfs_attr_*`):

- `spill_creates_total` — cumulative count of successful creates
  that landed on a non-parent tier.
- `spill_creates_failed_all_tiers` — cumulative count of creates
  that hit `-ENOSPC` on every tier.
- `any_spill_since_mount` — boolean, set when the first spill
  succeeds, never cleared until umount. Userspace reads this to
  decide whether `rsync --delete` is safe.

One new netlink event, `SMOOTHFS_CMD_SPILL` (next command id),
emitted on every spill. Payload: pool_uuid, oid, source_tier,
dest_tier, size. tierd uses this to update its own stats and to
trigger a faster-than-usual placement run on tier 0.

## 13. Rollout

1. Ship spill behind a mount option `spill=on` (default off in
   Phase 3.0). Operators enable it per-pool when they have a
   configuration with tier 1 capacity >> tier 0 and a workload
   that routinely overruns tier 0 (backup pools).
2. Collect telemetry for two release cycles. If
   `spill_creates_failed_all_tiers > 0` is ever observed on a
   production pool, revisit admission thresholds.
3. Flip default to `spill=on` in Phase 3.2 after telemetry clears.
4. Remove the mount option in Phase 3.3 (spill is always on; `off`
   no longer useful).

## 14. Open questions

- **High-water admission.** Should spill trigger at
  `f_bavail/f_blocks < 0.05` (5 %) even without an actual
  `-ENOSPC`? Proactively spilling avoids tail-end failures on
  large writes, at the cost of using tier 1 before strictly
  necessary. Recommended: yes, with the threshold as a mount
  option. Pinning the default to 2 % (leave 2 % free for XFS
  metadata headroom).
- **Reserved-space awareness.** XFS reserves ~5 % for root; the
  visible `f_bavail` already excludes this, so we're fine. Need
  to verify ZFS `f_bavail` excludes its own reservation
  consistently before relying on it for admission.
- **Placement record batching.** On a backup that spills many
  small files quickly, the placement log file gets one record
  per file. At 10 k files/s that's 640 kB/s of log writes, which
  the existing log infrastructure handles — but it's worth
  sanity-checking against a real backup trace once Phase 3.0
  ships.

## 15. Related follow-up work not in this proposal

- **Phase 2 union readdir.** Prerequisite for spill to be
  invisible-to-userspace. Separate proposal.
- **Phase 3 subtree reconcile.** Periodic background pass that
  merges non-canonical duplicate directories up to the canonical
  tier once the canonical tier has room. Frees empty tier-1 dir
  trees created by spill.
- **Phase 3 admission-aware planner.** Userspace tierd planner
  learns to aggressively demote cold tier-0 data when tier 0 is
  above high-water, reducing how often spill fires in the first
  place. Orthogonal; this proposal's kernel-side spill is still
  the safety net.
