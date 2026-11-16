package auth

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
)

// ChangeTrustedDevice follows the portal's normal add or full-quota replacement
// contract. A replacement sends only untrustIdList, never idList or another
// terminal's identity as the authenticated device.
func (s *Session) ChangeTrustedDevice(id string, replace bool) error {
	if id == "" {
		return fmt.Errorf("missing device record ID")
	}
	key := "idList"
	if replace {
		key = "untrustIdList"
	}
	body, _ := json.Marshal(map[string][]string{key: {id}})
	req, err := http.NewRequest("POST", s.baseURL+"/passport/v1/security/trustDevice?"+WithSharedParams(nil).Encode(), bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", UserAgent)
	req.Header.Set("Content-Type", "application/json;charset=utf-8")
	req.Header.Set("x-csrf-token", s.csrfToken)
	req.Header.Set("x-sdp-traceid", s.randSdpId())
	resp, err := s.client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("device mutation HTTP %d", resp.StatusCode)
	}
	body, err = io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024+1))
	if err != nil {
		return err
	}
	if len(body) > 2*1024*1024 {
		return fmt.Errorf("device response exceeds limit")
	}
	var result struct {
		Code    *int   `json:"code"`
		Message string `json:"message"`
		Data    struct {
			NextService string `json:"nextService"`
		} `json:"data"`
	}
	if json.Unmarshal(body, &result) != nil || result.Code == nil {
		return fmt.Errorf("invalid device mutation response")
	}
	if *result.Code != 0 {
		return fmt.Errorf("trustDevice failed with code %d: %s", *result.Code, result.Message)
	}
	if result.Data.NextService != "" {
		return fmt.Errorf("device additional authentication required")
	}
	return nil
}

// TrustedDeviceData preserves the device list discarded by QueryDevice.
func (s *Session) TrustedDeviceData() (json.RawMessage, error) {
	params := WithSharedParams(url.Values{"status": {"trust"}})
	req, err := http.NewRequest("GET", s.baseURL+"/passport/v1/security/queryDevice?"+params.Encode(), nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", UserAgent)
	req.Header.Set("Content-Type", "application/json;charset=utf-8")
	req.Header.Set("x-csrf-token", s.csrfToken)
	req.Header.Set("x-sdp-traceid", s.randSdpId())
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("device query HTTP %d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 2*1024*1024+1))
	if err != nil {
		return nil, err
	}
	if len(body) > 2*1024*1024 {
		return nil, fmt.Errorf("device response exceeds limit")
	}
	var response struct {
		Code    *int            `json:"code"`
		Message string          `json:"message"`
		Data    json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(body, &response); err != nil {
		return nil, fmt.Errorf("invalid device response")
	}
	if response.Code == nil {
		return nil, fmt.Errorf("missing device response code")
	}
	if *response.Code != 0 {
		return nil, fmt.Errorf("queryDevice failed with code %d: %s", *response.Code, response.Message)
	}
	if len(response.Data) == 0 || response.Data[0] != '{' {
		return nil, fmt.Errorf("invalid device list data")
	}
	return response.Data, nil
}
