package smoothfs

import (
	"encoding/binary"
	"testing"

	"github.com/google/uuid"
	"github.com/mdlayher/genetlink"
	"github.com/mdlayher/netlink"
)

func TestDecodeEventMountReadyCarriesSpillFlag(t *testing.T) {
	poolUUID := uuid.MustParse("11111111-2222-4333-8444-555555555555")
	data, err := netlink.MarshalAttributes([]netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
		{Type: AttrPoolName, Data: []byte("pool-a\x00")},
		{Type: AttrAnySpillSinceMount, Data: []byte{1}},
	})
	if err != nil {
		t.Fatalf("MarshalAttributes: %v", err)
	}

	ev, err := DecodeEvent(genetlink.Message{
		Header: genetlink.Header{Command: EventMountReady},
		Data:   data,
	})
	if err != nil {
		t.Fatalf("DecodeEvent: %v", err)
	}
	if ev == nil {
		t.Fatal("DecodeEvent returned nil event")
	}
	if ev.PoolUUID != poolUUID {
		t.Fatalf("PoolUUID = %s, want %s", ev.PoolUUID, poolUUID)
	}
	if ev.PoolName != "pool-a" {
		t.Fatalf("PoolName = %q, want pool-a", ev.PoolName)
	}
	if !ev.AnySpillSinceMount {
		t.Fatal("AnySpillSinceMount = false, want true")
	}
}

func TestDecodeEventSpill(t *testing.T) {
	poolUUID := uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")
	var oid [OIDLen]byte
	copy(oid[:], []byte("0123456789abcdef"))
	size := uint64(123456)
	sizeLE := make([]byte, 8)
	binary.LittleEndian.PutUint64(sizeLE, size)

	data, err := netlink.MarshalAttributes([]netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
		{Type: AttrAnySpillSinceMount, Data: []byte{1}},
		{Type: AttrObjectID, Data: oid[:]},
		{Type: AttrCurrentTier, Data: []byte{0}},
		{Type: AttrIntendedTier, Data: []byte{1}},
		{Type: AttrSizeBytes, Data: sizeLE},
	})
	if err != nil {
		t.Fatalf("MarshalAttributes: %v", err)
	}

	ev, err := DecodeEvent(genetlink.Message{
		Header: genetlink.Header{Command: EventSpill},
		Data:   data,
	})
	if err != nil {
		t.Fatalf("DecodeEvent: %v", err)
	}
	if ev == nil || ev.Spill == nil {
		t.Fatal("DecodeEvent did not populate spill event")
	}
	if ev.PoolUUID != poolUUID {
		t.Fatalf("PoolUUID = %s, want %s", ev.PoolUUID, poolUUID)
	}
	if !ev.AnySpillSinceMount {
		t.Fatal("AnySpillSinceMount = false, want true")
	}
	if ev.Spill.OID != oid {
		t.Fatalf("OID = %x, want %x", ev.Spill.OID, oid)
	}
	if ev.Spill.SourceTier != 0 || ev.Spill.DestTier != 1 {
		t.Fatalf("tiers = %d->%d, want 0->1", ev.Spill.SourceTier, ev.Spill.DestTier)
	}
	if ev.Spill.SizeBytes != size {
		t.Fatalf("SizeBytes = %d, want %d", ev.Spill.SizeBytes, size)
	}
}
