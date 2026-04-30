package smoothfs

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sync"

	"github.com/google/uuid"
	"github.com/mdlayher/genetlink"
	"github.com/mdlayher/netlink"
)

// Client is a thin generic-netlink wrapper for the "smoothfs" family.
type Client struct {
	cmd    *genetlink.Conn
	sub    *genetlink.Conn
	family genetlink.Family
	cmdMu  sync.Mutex
}

func Open() (*Client, error) {
	cmd, err := genetlink.Dial(nil)
	if err != nil {
		return nil, fmt.Errorf("dial cmd: %w", err)
	}
	fam, err := cmd.GetFamily(GenlFamilyName)
	if err != nil {
		cmd.Close()
		return nil, fmt.Errorf("%w: get smoothfs family: %v", ErrNotLoaded, err)
	}

	sub, err := genetlink.Dial(nil)
	if err != nil {
		cmd.Close()
		return nil, fmt.Errorf("dial sub: %w", err)
	}
	for _, g := range fam.Groups {
		if g.Name == GenlMcastGroup {
			if err := sub.JoinGroup(g.ID); err != nil {
				cmd.Close()
				sub.Close()
				return nil, fmt.Errorf("join multicast group %q: %w", g.Name, err)
			}
		}
	}
	return &Client{cmd: cmd, sub: sub, family: fam}, nil
}

func (c *Client) Close() error {
	if c == nil {
		return nil
	}
	var err error
	if c.cmd != nil {
		err = c.cmd.Close()
	}
	if c.sub != nil {
		if e := c.sub.Close(); e != nil && err == nil {
			err = e
		}
	}
	return err
}

func (c *Client) send(cmd uint8, attrs []netlink.Attribute) (genetlink.Message, error) {
	msg := genetlink.Message{
		Header: genetlink.Header{Command: cmd, Version: GenlFamilyVersion},
	}
	if len(attrs) > 0 {
		data, err := netlink.MarshalAttributes(attrs)
		if err != nil {
			return genetlink.Message{}, fmt.Errorf("marshal attrs: %w", err)
		}
		msg.Data = data
	}
	flags := netlink.Request | netlink.Acknowledge
	c.cmdMu.Lock()
	rsp, err := c.cmd.Execute(msg, c.family.ID, flags)
	c.cmdMu.Unlock()
	if err != nil {
		return genetlink.Message{}, err
	}
	if len(rsp) == 0 {
		return genetlink.Message{}, nil
	}
	return rsp[0], nil
}

func (c *Client) Receive() ([]genetlink.Message, error) {
	msgs, _, err := c.sub.Receive()
	return msgs, err
}

func (c *Client) MovePlan(poolUUID uuid.UUID, oid [OIDLen]byte, destTier uint8, seq uint64) error {
	return c.MovePlanForce(poolUUID, oid, destTier, seq, false)
}

func (c *Client) MovePlanForce(poolUUID uuid.UUID, oid [OIDLen]byte, destTier uint8, seq uint64, force bool) error {
	attrs := []netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
		{Type: AttrObjectID, Data: oid[:]},
		{Type: AttrIntendedTier, Data: []byte{destTier}},
		{Type: AttrTransactionSeq, Data: u64le(seq)},
	}
	if force {
		attrs = append(attrs, netlink.Attribute{Type: AttrForce, Data: []byte{1}})
	}
	_, err := c.send(CmdMovePlan, attrs)
	return err
}

func (c *Client) MoveCutover(poolUUID uuid.UUID, oid [OIDLen]byte, seq uint64) error {
	_, err := c.send(CmdMoveCutover, []netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
		{Type: AttrObjectID, Data: oid[:]},
		{Type: AttrTransactionSeq, Data: u64le(seq)},
	})
	return err
}

type InspectResult struct {
	OID             [OIDLen]byte
	CurrentTier     uint8
	IntendedTier    uint8
	MovementState   MovementState
	PinState        PinState
	Generation      uint32
	TransactionSeq  uint64
	RelPath         string
	CurrentTierPath string
}

func (c *Client) Inspect(poolUUID uuid.UUID, oid [OIDLen]byte) (*InspectResult, error) {
	rsp, err := c.send(CmdInspect, []netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
		{Type: AttrObjectID, Data: oid[:]},
	})
	if err != nil {
		return nil, err
	}
	attrs, err := netlink.UnmarshalAttributes(rsp.Data)
	if err != nil {
		return nil, fmt.Errorf("unmarshal inspect: %w", err)
	}
	out := &InspectResult{}
	for _, a := range attrs {
		switch a.Type {
		case AttrObjectID:
			if len(a.Data) == OIDLen {
				copy(out.OID[:], a.Data)
			}
		case AttrCurrentTier:
			if len(a.Data) >= 1 {
				out.CurrentTier = a.Data[0]
			}
		case AttrIntendedTier:
			if len(a.Data) >= 1 {
				out.IntendedTier = a.Data[0]
			}
		case AttrMovementState:
			if len(a.Data) >= 1 {
				out.MovementState = stateFromKernel(a.Data[0])
			}
		case AttrPinState:
			if len(a.Data) >= 1 {
				out.PinState = pinFromKernel(a.Data[0])
			}
		case AttrGeneration:
			if len(a.Data) >= 4 {
				out.Generation = binary.LittleEndian.Uint32(a.Data)
			}
		case AttrTransactionSeq:
			if len(a.Data) >= 8 {
				out.TransactionSeq = binary.LittleEndian.Uint64(a.Data)
			}
		case AttrRelPath:
			out.RelPath = nullTermString(a.Data)
		case AttrTierPath:
			out.CurrentTierPath = nullTermString(a.Data)
		}
	}
	return out, nil
}

func nullTermString(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}

func (c *Client) Quiesce(poolUUID uuid.UUID) error {
	_, err := c.send(CmdQuiesce, []netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
	})
	return err
}

func (c *Client) RevokeMappings(poolUUID uuid.UUID, oid [OIDLen]byte) error {
	_, err := c.send(CmdRevokeMappings, []netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
		{Type: AttrObjectID, Data: oid[:]},
	})
	return err
}

func (c *Client) Reconcile(poolUUID uuid.UUID, reason string) error {
	attrs := []netlink.Attribute{
		{Type: AttrPoolUUID, Data: poolUUID[:]},
	}
	if reason != "" {
		attrs = append(attrs, netlink.Attribute{
			Type: AttrReconcileReason, Data: []byte(reason + "\x00"),
		})
	}
	_, err := c.send(CmdReconcile, attrs)
	return err
}

func u64le(v uint64) []byte {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint64(b, v)
	return b
}

// ErrNotLoaded is returned by Open if the smoothfs kernel module is
// not loaded (the genetlink family doesn't exist).
var ErrNotLoaded = errors.New("smoothfs kernel module not loaded")
