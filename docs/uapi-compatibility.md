# smoothfs UAPI compatibility policy

This document defines compatibility rules for the smoothfs userspace ABI:

- generic-netlink family, commands, attributes, and multicast events
- kernel/userspace movement and pin state numeric values
- fixed binary records carried inside netlink attributes
- NFS file-handle type identifiers exposed by the kernel module

The authoritative declarations are:

- Kernel UAPI header: `src/smoothfs/uapi_smoothfs.h`
- Go userspace mirror: `uapi.go`
- Go request/response implementation: `client.go`
- Go multicast decoder: `events.go`

Any UAPI change must update the C and Go declarations in the same commit unless
the change is intentionally kernel-only and documented as such.

## Compatibility Levels

### Compatible Changes

These changes may keep `SMOOTHFS_GENL_VERSION` and `GenlFamilyVersion` at the
same value:

- Add a new command at the end of the command enum.
- Add a new multicast event at the end of the command enum.
- Add a new attribute at the end of the attribute enum.
- Add an optional attribute to an existing request, response, or event.
- Add a new enum string in Go for higher-level display, as long as the kernel
  numeric value is appended and older values keep their meaning.
- Widen implementation behavior behind an existing optional attribute without
  changing its type, length, or requiredness.

Old userspace must be able to ignore compatible kernel additions. Old kernels
must reject or ignore compatible userspace additions without corrupting state.

### Incompatible Changes

These changes require a version bump and a release note:

- Renumbering or reusing any command id, attribute id, movement-state id,
  pin-state id, file-handle type, or fixed binary field.
- Changing the type, byte order, width, or signedness of an existing attribute.
- Changing a previously optional attribute into a required attribute on an
  existing command.
- Removing an attribute that existing userspace can observe.
- Changing the binary layout or size of `struct smoothfs_heat_sample_record`.
- Changing the semantic meaning of an existing movement or pin state.
- Changing the generic-netlink family name or multicast group name.

If an incompatible change cannot be represented cleanly with a version bump,
register a new family name instead of overloading `smoothfs`.

## Numbering Rules

- `UNSPEC` entries stay at `0`.
- New command ids are appended after `SMOOTHFS_EVENT_SPILL`.
- New attribute ids are appended after the current final published attribute.
- Numeric gaps are reserved forever once published.
- Deleted features leave their ids reserved; ids are never recycled.
- Kernel `SMOOTHFS_CMD_MAX` and `SMOOTHFS_ATTR_MAX` must continue to derive from
  the final enum sentinel.
- Go constants must mirror the exact numeric values from the C header.

## Versioning Rules

The current generic-netlink version is:

```text
SMOOTHFS_GENL_VERSION = 1
GenlFamilyVersion    = 1
```

For compatible additions, keep the version unchanged and make the new field
optional for at least one release. For incompatible changes:

1. Increment `SMOOTHFS_GENL_VERSION` in `src/smoothfs/uapi_smoothfs.h`.
2. Increment `GenlFamilyVersion` in `uapi.go`.
3. Update request construction in `client.go`.
4. Update event/response decoding in `events.go`.
5. Add compatibility notes to `docs/smoothfs-support-matrix.md`.
6. Add tests or runtime harness coverage for old/new behavior where practical.

## Attribute Encoding

Integer attributes are emitted with the kernel `nla_put_u*` helpers. The
supported deployment architectures are little-endian today, and the Go decoder
reads fixed-width integer payloads as little-endian. Adding a big-endian target
requires an explicit compatibility review of every integer decoder.

| Attribute | Id | Type | Required semantics |
| --- | ---: | --- | --- |
| `POOL_UUID` | 1 | binary, 16 bytes | Smoothfs pool UUID. Required for pool-scoped commands. |
| `POOL_NAME` | 2 | nul-terminated string, max 63 | Human-readable pool name in mount-ready events. |
| `FSID` | 3 | `u32` | Kernel filesystem id for the mount. |
| `TIER_RANK` | 4 | `u8` | Kernel-visible tier rank. |
| `TIER_CAPS` | 5 | `u32` bitmap | Lower-tier capability bits. |
| `TIER_PATH` | 6 | string | Lower-tier path or current object lower path. |
| `TIER_ID` | 7 | string, max 64 | Control-plane `tier_targets.id` when used. |
| `OBJECT_ID` | 8 | binary, 16 bytes | Smoothfs object id. |
| `GENERATION` | 9 | `u32` | Cutover generation. |
| `MOVEMENT_STATE` | 10 | `u8` enum | Kernel movement state id. |
| `CURRENT_TIER` | 11 | `u8` | Current authoritative tier rank. |
| `INTENDED_TIER` | 12 | `u8` | Planned destination tier rank. |
| `TRANSACTION_SEQ` | 13 | `u64` | Control-plane movement transaction sequence. |
| `PIN_STATE` | 14 | `u8` enum | Kernel pin state id. |
| `HEAT_SAMPLE_BLOB` | 15 | binary | Packed `smoothfs_heat_sample_record` entries. |
| `CHECKPOINT_SEQ` | 16 | `u64` | Reserved checkpoint sequence surface. |
| `RECONCILE_REASON` | 17 | string, max 255 | Optional operator reconciliation reason. |
| `TIERS` | 18 | nested | Mount-ready tier list. |
| `REL_PATH` | 19 | string | Object path relative to the smoothfs namespace. |
| `FORCE` | 20 | `u8` boolean | Optional `MOVE_PLAN` pin bypass flag. |
| `SIZE_BYTES` | 21 | `u64` | Spill event object size. |
| `ANY_SPILL_SINCE_MOUNT` | 22 | `u8` boolean | Mount or spill state flag. |
| `WRITE_SEQ` | 23 | `u64` | Data-change sequence for verified cutover. |
| `RANGE_STAGED` | 24 | `u8` boolean | Inspect response flag for active range-staged bytes. |

Do not narrow the kernel `nla_policy` for an existing string or binary
attribute unless all supported userspace versions are known to fit.

## Command Contract

| Command | Id | Permission | Required request attributes | Response or behavior |
| --- | ---: | --- | --- | --- |
| `REGISTER_POOL` | 1 | admin | none today | Accepted as a no-op; mount options carry tier paths. |
| `POLICY_PUSH` | 2 | admin | reserved | Currently returns `-ENOSYS`. |
| `MOVE_PLAN` | 3 | admin | `POOL_UUID`, `OBJECT_ID`, `INTENDED_TIER`, `TRANSACTION_SEQ` | Prepares kernel movement state. Optional `FORCE`. |
| `TIER_DOWN` | 4 | admin | reserved | Accepted as a placeholder for tier quarantine. |
| `RECONCILE` | 5 | admin | `POOL_UUID` | Clears quiesce and mapping-quiesce state; optional reason. |
| `QUIESCE` | 6 | admin | `POOL_UUID` | Blocks new movement transitions for the pool. |
| `INSPECT` | 7 | unprivileged read | `POOL_UUID`, `OBJECT_ID` | Replies with object placement, state, sequence, and paths. |
| `REPROBE` | 8 | admin | reserved | Currently returns `-ENOSYS`. |
| `MOVE_CUTOVER` | 13 | admin | `POOL_UUID`, `OBJECT_ID`, `TRANSACTION_SEQ` | Commits movement; optional `WRITE_SEQ` guard. |
| `REVOKE_MAPPINGS` | 14 | admin | `POOL_UUID`, `OBJECT_ID` | Revokes writable shared mappings for one object. |

Command ids `9`, `10`, `11`, `12`, and `15` are multicast event ids, not
request commands.

## Event Contract

All multicast events use family `smoothfs`, group `events`, and version `1`.
Every event may include new optional attributes in later compatible releases.

| Event | Id | Required attributes | Optional or contextual attributes |
| --- | ---: | --- | --- |
| `MOUNT_READY` | 9 | `POOL_UUID`, `POOL_NAME`, `FSID`, `TIERS` | `ANY_SPILL_SINCE_MOUNT` |
| `HEAT_SAMPLE` | 10 | `POOL_UUID`, `HEAT_SAMPLE_BLOB` | none today |
| `MOVE_STATE` | 11 | `POOL_UUID`, `OBJECT_ID`, `MOVEMENT_STATE`, `TRANSACTION_SEQ` | none today |
| `TIER_FAULT` | 12 | `POOL_UUID`, `POOL_NAME`, `FSID`, `TIER_RANK` | `ANY_SPILL_SINCE_MOUNT` |
| `SPILL` | 15 | `POOL_UUID`, `OBJECT_ID`, `CURRENT_TIER`, `INTENDED_TIER`, `SIZE_BYTES` | `ANY_SPILL_SINCE_MOUNT` |

Userspace event decoders must ignore unknown attributes. Unknown event command
ids should be ignored rather than treated as fatal stream corruption.

## Fixed Binary Records

`SMOOTHFS_ATTR_HEAT_SAMPLE_BLOB` is a concatenation of little-endian
`struct smoothfs_heat_sample_record` entries. Each entry is exactly 56 bytes:

```text
oid[16]
open_count_delta u32
reserved u32
read_bytes_delta u64
write_bytes_delta u64
last_access_ns u64
sample_window_ns u64
```

The Go mirror uses `HeatSampleRecordSize == 56` plus compile-time size checks.
Changing this layout is incompatible. Add a new attribute for a replacement
record format if the heat sample payload must grow.

The kernel currently emits up to 256 heat records per message.

## State Enum Contract

Movement and pin states cross three boundaries: kernel enums, Go string
constants, and SQLite CHECK values documented in
`docs/control-plane-schema.md`.

Rules:

- Existing numeric state values must not change.
- Existing Go string names must not change without a migration of persisted
  database rows.
- New states are appended and must define recovery behavior before release.
- Unknown movement-state bytes decode to `failed` in current Go userspace, so
  new kernel states should not be emitted to old userspace unless treating them
  as failed is acceptable.
- Unknown pin-state bytes decode to `none` in current Go userspace, so new pin
  states require a compatibility review before kernel emission.

## File-Handle Type Contract

The NFS file-handle type ids are part of the kernel-facing UAPI:

```text
FILEID_SMOOTHFS_OID             = 0x53
FILEID_SMOOTHFS_OID_CONNECTABLE = 0x54
```

The payload layouts in `src/smoothfs/uapi_smoothfs.h` are fixed. New handle
formats must use new file-handle type ids and keep old decoders working.

## Review Checklist

Before merging a UAPI change:

- C and Go constants match by name and numeric value.
- `uapi_smoothfs.h` comments match actual struct sizes and event payloads.
- `client.go` sends only documented required attributes for old commands.
- `events.go` accepts old payloads and ignores new optional attributes.
- Existing command and attribute ids are not renumbered or reused.
- Any new required behavior has either a version bump or an additive fallback.
- `make verify` and `make kernel-build-debian` pass.
- Runtime harness coverage is added when the change affects mounted-kernel
  behavior.
