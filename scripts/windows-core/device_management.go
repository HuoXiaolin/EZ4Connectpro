package atrust

import (
	"encoding/json"
	"fmt"
	"strings"

	"github.com/mythologyli/zju-connect/client/atrust/auth"
)

type deviceManager interface {
	TrustedDeviceData() (json.RawMessage, error)
	ChangeTrustedDevice(string, bool) error
	UntrustDevice([]string) error
}

func applyDeviceAction(session deviceManager, action, target string) (json.RawMessage, error) {
	if action != "list" && action != "trust-self" && action != "untrust" && action != "replace-self" {
		return nil, fmt.Errorf("unsupported device action")
	}
	data, err := session.TrustedDeviceData()
	if err != nil {
		return nil, err
	}
	if action == "list" {
		return data, nil
	}
	var current struct {
		SelfID      string `json:"selfId"`
		Trusted     *bool  `json:"deviceTrusted"`
		Count       *int   `json:"currentTrustDeviceCount"`
		TrustConfig struct {
			Enable   bool `json:"enable"`
			SelfBind int  `json:"enableSelfBind"`
			Limit    int  `json:"limit"`
		} `json:"trustDeviceConfig"`
		Devices *[]struct {
			ID string `json:"id"`
		} `json:"data"`
	}
	if json.Unmarshal(data, &current) != nil || current.SelfID == "" || current.Trusted == nil || current.Devices == nil {
		return nil, fmt.Errorf("unsupported device list format")
	}
	if action == "trust-self" {
		if target != "" {
			return nil, fmt.Errorf("trust-self cannot target another device")
		}
		if *current.Trusted {
			return json.RawMessage(`{"changed":false}`), nil
		}
		err = session.ChangeTrustedDevice(current.SelfID, false)
	} else {
		if strings.TrimSpace(target) == "" {
			return nil, fmt.Errorf("missing device record ID")
		}
		matches := 0
		for _, item := range *current.Devices {
			if item.ID == target {
				matches++
			}
		}
		if matches != 1 {
			return nil, fmt.Errorf("device record changed; refresh the list")
		}
		if action == "replace-self" {
			if *current.Trusted || target == current.SelfID || !current.TrustConfig.Enable || current.TrustConfig.SelfBind != 1 || current.TrustConfig.Limit <= 0 || current.Count == nil || *current.Count != current.TrustConfig.Limit {
				return nil, fmt.Errorf("device replacement conditions changed; refresh the list")
			}
			// Official full-quota flow: one request replaces the chosen terminal
			// with the authenticated current terminal. Never retry as a plain unbind.
			err = session.ChangeTrustedDevice(target, true)
		} else {
			if !*current.Trusted {
				return nil, fmt.Errorf("current device is not trusted; use replace-self at full quota")
			}
			err = session.UntrustDevice([]string{target})
		}
	}
	if err != nil {
		return nil, err
	}
	return json.RawMessage(`{"changed":true}`), nil
}

// ManageDevices reuses the saved authenticated device identity. It never starts
// a tunnel, prompts for primary credentials, or creates a new device identity.
func ManageDevices(server string, port int, data []byte, action, target string) (json.RawMessage, error) {
	var saved auth.ClientAuthData
	if err := json.Unmarshal(data, &saved); err != nil {
		return nil, fmt.Errorf("invalid saved login data")
	}
	if saved.DeviceID == "" || len(saved.Cookies) == 0 {
		return nil, fmt.Errorf("saved login required")
	}
	if port != 443 {
		server = fmt.Sprintf("%s:%d", server, port)
	}
	dialer, err := newUnderlayDialer("", false, "")
	if err != nil {
		return nil, err
	}
	defer dialer.Close()
	session := auth.NewSession(server, nil, dialer.DialContext)
	if _, err = session.Login(nil, auth.LoginOptions{DeviceID: saved.DeviceID, Cookies: saved.Cookies}); err != nil {
		return nil, err
	}
	return applyDeviceAction(session, action, target)
}
