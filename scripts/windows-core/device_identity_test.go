//go:build windows

package auth

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"reflect"
	"strings"
	"testing"
)

func TestEZ4WindowsDeviceReport(t *testing.T) {
	hostname, err := os.Hostname()
	if err != nil || hostname == "" {
		t.Fatal("hostname unavailable")
	}
	for _, reply := range []string{`{"code":0}`, `{"code":75510008}`, `{}`, `<html>login</html>`} {
		calls := 0
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			calls++
			if r.Method != "POST" || r.URL.Path != "/controller/v1/user/reportEnv" || r.URL.Query().Get("platform") != "Linux" || r.Header.Get("x-csrf-token") != "fixture-csrf" {
				t.Error("incorrect environment request")
			}
			if ua := r.Header.Get("User-Agent"); !strings.Contains(ua, "aTrustTray-Linux-Plat-Ubuntu-x64") {
				t.Error("device metadata changed the supported wire protocol")
			}
			var payload struct {
				DeviceID string `json:"deviceId"`
				Env      struct {
					Endpoint struct {
						DeviceID string            `json:"device_id"`
						OS       map[string]string `json:"os"`
						Security json.RawMessage   `json:"security"`
					} `json:"endpoint"`
				} `json:"env"`
			}
			if json.NewDecoder(r.Body).Decode(&payload) != nil {
				t.Error("invalid JSON")
			}
			if payload.DeviceID != "stable-device" || payload.Env.Endpoint.DeviceID != "stable-device" {
				t.Error("device identity changed")
			}
			info := payload.Env.Endpoint.OS
			if info["hostname"] != hostname || info["family"] != "Windows" || info["version"] == "" || info["arch"] == "" || !strings.HasPrefix(info["sub_os"], "Windows") {
				t.Error("local hostname or Windows information missing")
			}
			if len(payload.Env.Endpoint.Security) != 0 {
				t.Error("fabricated security posture")
			}
			fmt.Fprint(w, reply)
		}))
		session := &Session{client: server.Client(), baseURL: server.URL, deviceID: "stable-device", csrfToken: "fixture-csrf"}
		err := session.reportCurrentEnvironment()
		server.Close()
		if (err == nil) != (reply == `{"code":0}`) || calls != 1 || session.deviceID != "stable-device" {
			t.Fatal("report acceptance or identity handling failed")
		}
	}
}

// Exercise the real CAS-to-reportEnv sequence, not just the already-logged-in
// shortcut. The mandatory request must remain compatible with the pinned core.
func TestEZ4FreshCASPreservesAuthenticationContract(t *testing.T) {
	for _, mandatoryRejected := range []bool{false, true} {
		for _, metadataCode := range []int{0, 10000008} {
			t.Run(fmt.Sprintf("reject=%v/metadata=%d", mandatoryRejected, metadataCode), func(t *testing.T) {
				var order []string
				server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
					order = append(order, r.URL.Path)
					switch r.URL.Path {
					case "/passport/v1/public/authConfig":
						fmt.Fprint(w, `{"data":{"isLogin":0,"csrfToken":"fixture","authServerInfoList":[{"authType":"auth/cas","loginDomain":"CAS"}]}}`)
					case "/passport/v1/auth/cas":
						if r.URL.Query().Get("ticket") != "cas-fixture" {
							t.Error("CAS ticket changed")
						}
						w.Header().Set("Location", "https://"+r.Host+"/portal/shortcut.html?data="+url.QueryEscape(`{"ticket":"portal-fixture"}`))
						w.WriteHeader(http.StatusFound)
					case "/controller/v1/public/reportEnv":
						var payload map[string]interface{}
						json.NewDecoder(r.Body).Decode(&payload)
						expected := map[string]interface{}{"ticket": "portal-fixture", "deviceId": "stable-device", "env": map[string]interface{}{"endpoint": map[string]interface{}{"device_id": "stable-device", "device": map[string]interface{}{"type": "browser"}}}}
						compatible := r.URL.Query().Get("platform") == "Linux" && strings.Contains(r.Header.Get("User-Agent"), "aTrustTray-Linux-Plat-Ubuntu-x64") && reflect.DeepEqual(payload, expected)
						if !compatible || mandatoryRejected {
							fmt.Fprint(w, `{"code":10000008,"message":"interface sig verify failed"}`)
						} else {
							fmt.Fprint(w, `{"code":0}`)
						}
					case "/passport/v1/auth/authCheck":
						http.SetCookie(w, &http.Cookie{Name: "sid", Value: "fresh-fixture", Path: "/"})
						fmt.Fprint(w, `{"code":0,"data":{}}`)
					case "/passport/v1/user/onlineInfo":
						fmt.Fprint(w, `{"code":0,"data":{"username":"fixture"}}`)
					case "/controller/v1/user/reportEnv":
						cookie, err := r.Cookie("sid")
						if err != nil || cookie.Value != "fresh-fixture" {
							t.Error("metadata sent before authentication completed")
						}
						fmt.Fprintf(w, `{"code":%d}`, metadataCode)
					default:
						t.Error("unexpected authentication request")
						http.NotFound(w, r)
					}
				}))
				defer server.Close()
				session := NewSession(strings.TrimPrefix(server.URL, "https://"), nil)
				result, err := session.Login(CASLogin{Domain: "CAS", Ticket: "cas-fixture"}, LoginOptions{DeviceID: "stable-device", ReportDeviceMetadata: true})
				want := []string{"/passport/v1/public/authConfig", "/passport/v1/auth/cas", "/passport/v1/public/authConfig", "/controller/v1/public/reportEnv"}
				if mandatoryRejected {
					if err == nil || !strings.Contains(err.Error(), "10000008") || result.SID != "" {
						t.Fatal("mandatory authentication rejection was ignored")
					}
				} else {
					if err != nil || result.SID != "fresh-fixture" {
						t.Fatalf("CAS succeeded but legacy reportEnv contract failed: %v", err)
					}
					want = append(want, "/passport/v1/auth/authCheck", "/passport/v1/user/onlineInfo", "/controller/v1/user/reportEnv")
				}
				if !reflect.DeepEqual(order, want) || session.deviceID != "stable-device" {
					t.Fatalf("authentication order or identity changed: %v", order)
				}
			})
		}
	}
}

func TestEZ4WindowsDeviceReportPreservesCachedLogin(t *testing.T) {
	for _, optIn := range []bool{false, true} {
		posts := 0
		server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch r.URL.Path {
			case "/passport/v1/public/authConfig":
				fmt.Fprint(w, `{"data":{"isLogin":1,"csrfToken":"fixture"}}`)
			case "/controller/v1/user/reportEnv":
				posts++
				fmt.Fprint(w, `{"code":75510008}`)
			case "/passport/v1/user/onlineInfo":
				fmt.Fprint(w, `{"code":0,"data":{"username":"fixture"}}`)
			default:
				t.Error("unexpected login or trust mutation")
				http.NotFound(w, r)
			}
		}))
		host := strings.TrimPrefix(server.URL, "https://")
		session := NewSession(host, nil)
		result, err := session.Login(nil, LoginOptions{DeviceID: "stable-device", ReportDeviceMetadata: optIn, Cookies: []Cookie{{Host: host, Scheme: "https", Name: "sid", Value: "cached-fixture"}}})
		server.Close()
		if err != nil || result.SID != "cached-fixture" || session.deviceID != "stable-device" || (posts == 1) != optIn {
			t.Fatal("metadata rejection disrupted login or read-only caller reported metadata", err, posts)
		}
	}
}
