package smoothfs

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/google/uuid"
	"github.com/mdlayher/genetlink"
	"github.com/mdlayher/netlink"
)

// Event is the tagged union of multicast messages from the kernel.
type Event struct {
	PoolUUID    uuid.UUID
	Type        uint8
	PoolName    string
	HeatSamples []HeatSampleRecord
	Tiers       []MountedTier
	Move        *MoveStateEvent
	Tier        *TierFaultEvent
}

type MoveStateEvent struct {
	OID            [OIDLen]byte
	NewState       MovementState
	TransactionSeq uint64
}

type TierFaultEvent struct {
	TierRank uint8
}

func DecodeEvent(msg genetlink.Message) (*Event, error) {
	cmd := msg.Header.Command
	switch cmd {
	case EventMountReady, EventHeatSample, EventMoveState, EventTierFault:
	default:
		return nil, nil
	}
	attrs, err := netlink.UnmarshalAttributes(msg.Data)
	if err != nil {
		return nil, fmt.Errorf("unmarshal event %d: %w", cmd, err)
	}
	ev := &Event{Type: cmd}
	for _, a := range attrs {
		switch a.Type {
		case AttrPoolUUID:
			if len(a.Data) == 16 {
				copy(ev.PoolUUID[:], a.Data)
			}
		case AttrPoolName:
			ev.PoolName = nullTermString(a.Data)
		case AttrHeatSampleBlob:
			records, err := parseHeatBlob(a.Data)
			if err != nil {
				return nil, err
			}
			ev.HeatSamples = records
		case AttrTiers:
			tiers, err := parseTiers(a.Data)
			if err != nil {
				return nil, err
			}
			ev.Tiers = tiers
		case AttrObjectID:
			if ev.Move == nil {
				ev.Move = &MoveStateEvent{}
			}
			if len(a.Data) == OIDLen {
				copy(ev.Move.OID[:], a.Data)
			}
		case AttrMovementState:
			if ev.Move == nil {
				ev.Move = &MoveStateEvent{}
			}
			if len(a.Data) >= 1 {
				ev.Move.NewState = stateFromKernel(a.Data[0])
			}
		case AttrTransactionSeq:
			if ev.Move == nil {
				ev.Move = &MoveStateEvent{}
			}
			if len(a.Data) >= 8 {
				ev.Move.TransactionSeq = binary.LittleEndian.Uint64(a.Data)
			}
		case AttrTierRank:
			if ev.Tier == nil {
				ev.Tier = &TierFaultEvent{}
			}
			if len(a.Data) >= 1 {
				ev.Tier.TierRank = a.Data[0]
			}
		}
	}
	return ev, nil
}

func parseHeatBlob(data []byte) ([]HeatSampleRecord, error) {
	if len(data)%HeatSampleRecordSize != 0 {
		return nil, fmt.Errorf("heat blob length %d is not a multiple of %d",
			len(data), HeatSampleRecordSize)
	}
	n := len(data) / HeatSampleRecordSize
	out := make([]HeatSampleRecord, n)
	r := bytes.NewReader(data)
	if err := binary.Read(r, binary.LittleEndian, out); err != nil {
		return nil, fmt.Errorf("decode heat records: %w", err)
	}
	return out, nil
}

func parseTiers(data []byte) ([]MountedTier, error) {
	entries, err := netlink.UnmarshalAttributes(data)
	if err != nil {
		return nil, fmt.Errorf("unmarshal tier list: %w", err)
	}
	out := make([]MountedTier, 0, len(entries))
	for _, entry := range entries {
		attrs, err := netlink.UnmarshalAttributes(entry.Data)
		if err != nil {
			return nil, fmt.Errorf("unmarshal tier entry: %w", err)
		}
		var tier MountedTier
		for _, a := range attrs {
			switch a.Type {
			case AttrTierRank:
				if len(a.Data) >= 1 {
					tier.Rank = a.Data[0]
				}
			case AttrTierPath:
				tier.Path = nullTermString(a.Data)
			case AttrTierCaps:
				if len(a.Data) >= 4 {
					tier.Caps = binary.LittleEndian.Uint32(a.Data)
				}
			}
		}
		out = append(out, tier)
	}
	return out, nil
}
