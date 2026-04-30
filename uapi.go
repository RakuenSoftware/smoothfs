// Package smoothfs provides the low-coupling userspace contract for the
// smoothfs kernel module. It intentionally carries the generic-netlink
// client, event decoder, and managed-pool helpers that consumers can
// import as a versioned dependency.
package smoothfs

import "unsafe"

const (
	GenlFamilyName    = "smoothfs"
	GenlFamilyVersion = 1
	GenlMcastGroup    = "events"
	OIDLen            = 16
)

// Generic netlink commands. Keep in sync with src/smoothfs/uapi_smoothfs.h.
const (
	CmdUnspec         uint8 = 0
	CmdRegisterPool   uint8 = 1
	CmdPolicyPush     uint8 = 2
	CmdMovePlan       uint8 = 3
	CmdTierDown       uint8 = 4
	CmdReconcile      uint8 = 5
	CmdQuiesce        uint8 = 6
	CmdInspect        uint8 = 7
	CmdReprobe        uint8 = 8
	EventMountReady   uint8 = 9
	EventHeatSample   uint8 = 10
	EventMoveState    uint8 = 11
	EventTierFault    uint8 = 12
	CmdMoveCutover    uint8 = 13
	CmdRevokeMappings uint8 = 14
	EventSpill        uint8 = 15
)

// Generic netlink attributes. Keep in sync with src/smoothfs/uapi_smoothfs.h.
const (
	AttrUnspec             uint16 = 0
	AttrPoolUUID           uint16 = 1
	AttrPoolName           uint16 = 2
	AttrFSID               uint16 = 3
	AttrTierRank           uint16 = 4
	AttrTierCaps           uint16 = 5
	AttrTierPath           uint16 = 6
	AttrTierID             uint16 = 7
	AttrObjectID           uint16 = 8
	AttrGeneration         uint16 = 9
	AttrMovementState      uint16 = 10
	AttrCurrentTier        uint16 = 11
	AttrIntendedTier       uint16 = 12
	AttrTransactionSeq     uint16 = 13
	AttrPinState           uint16 = 14
	AttrHeatSampleBlob     uint16 = 15
	AttrCheckpointSeq      uint16 = 16
	AttrReconcileReason    uint16 = 17
	AttrTiers              uint16 = 18
	AttrRelPath            uint16 = 19
	AttrForce              uint16 = 20
	AttrSizeBytes          uint16 = 21
	AttrAnySpillSinceMount uint16 = 22
	AttrWriteSeq           uint16 = 23
	AttrRangeStaged        uint16 = 24
)

// MovementState mirrors the on-disk movement_state CHECK column +
// kernel-internal enum.
type MovementState string

const (
	StatePlaced              MovementState = "placed"
	StatePlanAccepted        MovementState = "plan_accepted"
	StateDestinationReserved MovementState = "destination_reserved"
	StateCopyInProgress      MovementState = "copy_in_progress"
	StateCopyComplete        MovementState = "copy_complete"
	StateCopyVerified        MovementState = "copy_verified"
	StateCutoverInProgress   MovementState = "cutover_in_progress"
	StateSwitched            MovementState = "switched"
	StateCleanupInProgress   MovementState = "cleanup_in_progress"
	StateCleanupComplete     MovementState = "cleanup_complete"
	StateFailed              MovementState = "failed"
	StateStale               MovementState = "stale"
)

func stateFromKernel(b uint8) MovementState {
	switch b {
	case 0:
		return StatePlaced
	case 1:
		return StatePlanAccepted
	case 2:
		return StateDestinationReserved
	case 3:
		return StateCopyInProgress
	case 4:
		return StateCopyComplete
	case 5:
		return StateCopyVerified
	case 6:
		return StateCutoverInProgress
	case 7:
		return StateSwitched
	case 8:
		return StateCleanupInProgress
	case 9:
		return StateCleanupComplete
	case 10:
		return StateFailed
	case 11:
		return StateStale
	}
	return StateFailed
}

// PinState mirrors the on-disk pin_state CHECK column.
type PinState string

const (
	PinNone     PinState = "none"
	PinHot      PinState = "pin_hot"
	PinCold     PinState = "pin_cold"
	PinHardlink PinState = "pin_hardlink"
	PinLease    PinState = "pin_lease"
	PinLUN      PinState = "pin_lun"
)

func pinFromKernel(b uint8) PinState {
	switch b {
	case 0:
		return PinNone
	case 1:
		return PinHot
	case 2:
		return PinCold
	case 3:
		return PinHardlink
	case 4:
		return PinLease
	case 5:
		return PinLUN
	}
	return PinNone
}

// HeatSampleRecord matches the 56-byte on-wire layout in
// uapi_smoothfs.h.
type HeatSampleRecord struct {
	OID             [OIDLen]byte
	OpenCountDelta  uint32
	_               uint32
	ReadBytesDelta  uint64
	WriteBytesDelta uint64
	LastAccessNS    uint64
	SampleWindowNS  uint64
}

const HeatSampleRecordSize = 56

const _ = uintptr(HeatSampleRecordSize) - unsafe.Sizeof(HeatSampleRecord{})
const _ = unsafe.Sizeof(HeatSampleRecord{}) - uintptr(HeatSampleRecordSize)

type MountedTier struct {
	Rank uint8
	Path string
	Caps uint32
}
