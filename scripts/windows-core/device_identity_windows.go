//go:build windows

package auth

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"runtime"
	"strings"
	"time"

	"github.com/mythologyli/zju-connect/log"
	"golang.org/x/sys/windows"
)

// OS display metadata must not change the core's supported wire protocol.
// Keep the pinned upstream UserAgent/platform and mandatory login reportEnv.
func (s *Session) updateDeviceMetadata() {
	if err := s.reportCurrentEnvironment(); err != nil {
		log.Printf("Windows device information not updated; keeping current login: %v", err)
	} else {
		log.Println("Windows device information reported")
	}
}

// Field names match the official Windows environment reporter. Do not fabricate
// antivirus, domain, compliance or trust state: this only reports local identity.
func endpointEnvironment(deviceID string) map[string]interface{} {
	hostname, _ := os.Hostname()
	hostname = strings.TrimSpace(hostname)
	version := windows.RtlGetVersion()
	name := "Windows 10"
	if version.BuildNumber >= 22000 {
		name = "Windows 11"
	}
	if version.ProductType != 1 { // VER_NT_WORKSTATION
		name = "Windows Server"
	}
	arch := runtime.GOARCH
	if arch == "amd64" {
		arch = "x64"
	} else if arch == "386" {
		arch = "x86"
	}
	osInfo := map[string]interface{}{
		"family": "Windows", "version": fmt.Sprintf("%d.%d.%d", version.MajorVersion, version.MinorVersion, version.BuildNumber),
		"sub_os": name, "arch": arch,
	}
	if hostname != "" {
		osInfo["hostname"] = hostname
	}
	return map[string]interface{}{
		"device_id": deviceID, "device": map[string]interface{}{"type": "browser"}, "os": osInfo,
	}
}

// An already authenticated connection can refresh its name without logging out,
// changing the stable device ID, or binding/unbinding a trusted-device record.
// Login callers performing read-only device queries do not opt into this step.
func (s *Session) reportCurrentEnvironment() error {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	body, err := json.Marshal(map[string]interface{}{
		"deviceId": s.deviceID, "env": map[string]interface{}{"endpoint": endpointEnvironment(s.deviceID)},
	})
	if err != nil {
		return fmt.Errorf("cannot encode local device information")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost,
		s.baseURL+"/controller/v1/user/reportEnv?"+WithSharedParams(nil).Encode(), bytes.NewReader(body))
	if err != nil {
		return fmt.Errorf("invalid device information endpoint")
	}
	req.Header.Set("User-Agent", UserAgent)
	req.Header.Set("Content-Type", "application/json;charset=utf-8")
	req.Header.Set("x-csrf-token", s.csrfToken)
	req.Header.Set("x-sdp-traceid", s.randSdpId())
	resp, err := s.client.Do(req)
	if err != nil {
		return fmt.Errorf("device information request did not complete")
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("device information returned HTTP %d", resp.StatusCode)
	}
	var reply struct {
		Code *int `json:"code"`
	}
	if json.NewDecoder(io.LimitReader(resp.Body, 65536)).Decode(&reply) != nil || reply.Code == nil {
		return fmt.Errorf("invalid device information response")
	}
	if *reply.Code != 0 {
		return fmt.Errorf("device information rejected with code %d", *reply.Code)
	}
	return nil
}
