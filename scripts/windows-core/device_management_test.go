package atrust

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

type ez4DeviceSession struct {
	data                       json.RawMessage
	queryError, mutationError  error
	trusted, removed, replaced []string
	queries                    int
}

func (s *ez4DeviceSession) TrustedDeviceData() (json.RawMessage, error) {
	s.queries++
	return s.data, s.queryError
}
func (s *ez4DeviceSession) ChangeTrustedDevice(id string, replace bool) error {
	if replace {
		s.replaced = []string{id}
	} else {
		s.trusted = []string{id}
	}
	return s.mutationError
}
func (s *ez4DeviceSession) UntrustDevice(ids []string) error { s.removed = ids; return s.mutationError }

func TestEZ4DeviceActions(t *testing.T) {
	const list = `{"selfId":"this-device","deviceTrusted":false,"currentTrustDeviceCount":2,"trustDeviceConfig":{"enable":true,"enableSelfBind":1,"limit":2},"data":[{"id":"old-laptop"},{"id":"old-desktop"}]}`
	t.Run("list is read only", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(list)}
		if _, err := applyDeviceAction(s, "list", ""); err != nil || len(s.trusted)+len(s.removed) != 0 {
			t.Fatal("list mutated devices", err)
		}
	})
	t.Run("only requested record removed", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(strings.Replace(list, `"deviceTrusted":false`, `"deviceTrusted":true`, 1))}
		if _, err := applyDeviceAction(s, "untrust", "old-desktop"); err != nil || s.queries != 1 || len(s.removed) != 1 || s.removed[0] != "old-desktop" {
			t.Fatal("wrong device removal", err)
		}
	})
	t.Run("full quota replacement is a single managed trust request", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(list)}
		if _, err := applyDeviceAction(s, "replace-self", "old-desktop"); err != nil || s.queries != 1 || len(s.replaced) != 1 || s.replaced[0] != "old-desktop" || len(s.removed)+len(s.trusted) != 0 {
			t.Fatal("replacement did not use the portal contract", err)
		}
	})
	t.Run("replacement refuses changed quota policy identity or selection", func(t *testing.T) {
		for _, pair := range [][2]string{{`"deviceTrusted":false`, `"deviceTrusted":true`}, {`"enable":true`, `"enable":false`}, {`"enableSelfBind":1`, `"enableSelfBind":0`}, {`"currentTrustDeviceCount":2`, `"currentTrustDeviceCount":1`}, {`"currentTrustDeviceCount":2`, `"currentTrustDeviceCount":3`}, {`"limit":2`, `"limit":0`}} {
			s := &ez4DeviceSession{data: json.RawMessage(strings.Replace(list, pair[0], pair[1], 1))}
			if _, err := applyDeviceAction(s, "replace-self", "old-desktop"); err == nil || len(s.replaced)+len(s.removed)+len(s.trusted) != 0 {
				t.Fatal("stale replacement allowed")
			}
		}
		for _, id := range []string{"", "missing", "this-device", "old-laptop,old-desktop"} {
			s := &ez4DeviceSession{data: json.RawMessage(list)}
			if _, err := applyDeviceAction(s, "replace-self", id); err == nil || len(s.replaced) != 0 {
				t.Fatal("invalid replacement selection accepted")
			}
		}
	})
	t.Run("untrusted terminal cannot use plain unbind", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(list)}
		if _, err := applyDeviceAction(s, "untrust", "old-desktop"); err == nil || len(s.removed) != 0 {
			t.Fatal("plain unbind submitted from untrusted terminal")
		}
	})
	t.Run("fresh membership required", func(t *testing.T) {
		for _, id := range []string{"", "missing", "old-laptop,old-desktop", "this-device"} {
			s := &ez4DeviceSession{data: json.RawMessage(list)}
			if _, err := applyDeviceAction(s, "untrust", id); err == nil || len(s.removed) != 0 {
				t.Fatalf("unexpected removal for %q", id)
			}
		}
	})
	t.Run("self identity comes from server", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(list)}
		if _, err := applyDeviceAction(s, "trust-self", ""); err != nil || len(s.trusted) != 1 || s.trusted[0] != "this-device" {
			t.Fatal("wrong self identity", err)
		}
	})
	t.Run("cannot trust another device", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(list)}
		if _, err := applyDeviceAction(s, "trust-self", "old-laptop"); err == nil || len(s.trusted) != 0 {
			t.Fatal("allowed a different target")
		}
	})
	t.Run("already trusted is idempotent", func(t *testing.T) {
		s := &ez4DeviceSession{data: json.RawMessage(`{"selfId":"this-device","deviceTrusted":true,"data":[]}`)}
		result, err := applyDeviceAction(s, "trust-self", "")
		if err != nil || string(result) != `{"changed":false}` || len(s.trusted) != 0 {
			t.Fatal("unexpected second bind", err)
		}
	})
	t.Run("malformed and duplicate records rejected", func(t *testing.T) {
		for _, list := range []string{`{}`, `{"selfId":"s","deviceTrusted":false}`, `{"selfId":"s","deviceTrusted":false,"data":[{"id":"d"},{"id":"d"}]}`} {
			s := &ez4DeviceSession{data: json.RawMessage(list)}
			if _, err := applyDeviceAction(s, "untrust", "d"); err == nil || len(s.removed) != 0 {
				t.Fatal("invalid data permitted removal")
			}
		}
	})
	t.Run("server rejection preserved without retry", func(t *testing.T) {
		denied := errors.New("trustDevice failed with code 75500311: Failed")
		s := &ez4DeviceSession{data: json.RawMessage(list), mutationError: denied}
		if _, err := applyDeviceAction(s, "trust-self", ""); !errors.Is(err, denied) || s.queries != 1 {
			t.Fatal("lost server denial", err)
		}
	})
}
