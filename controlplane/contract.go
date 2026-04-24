package controlplane

import root "github.com/RakuenSoftware/smoothfs"

type Client = root.Client
type Event = root.Event
type MoveStateEvent = root.MoveStateEvent
type TierFaultEvent = root.TierFaultEvent
type SpillEvent = root.SpillEvent
type MovementState = root.MovementState
type PinState = root.PinState
type HeatSampleRecord = root.HeatSampleRecord
type MountedTier = root.MountedTier
type InspectResult = root.InspectResult

const (
	OIDLen                   = root.OIDLen
	EventMountReady          = root.EventMountReady
	EventHeatSample          = root.EventHeatSample
	EventMoveState           = root.EventMoveState
	EventTierFault           = root.EventTierFault
	EventSpill               = root.EventSpill
	StatePlaced              = root.StatePlaced
	StatePlanAccepted        = root.StatePlanAccepted
	StateDestinationReserved = root.StateDestinationReserved
	StateCopyInProgress      = root.StateCopyInProgress
	StateCopyComplete        = root.StateCopyComplete
	StateCopyVerified        = root.StateCopyVerified
	StateCutoverInProgress   = root.StateCutoverInProgress
	StateSwitched            = root.StateSwitched
	StateCleanupInProgress   = root.StateCleanupInProgress
	StateCleanupComplete     = root.StateCleanupComplete
	StateFailed              = root.StateFailed
	StateStale               = root.StateStale
)

var (
	Open         = root.Open
	DecodeEvent  = root.DecodeEvent
	ErrNotLoaded = root.ErrNotLoaded
)
