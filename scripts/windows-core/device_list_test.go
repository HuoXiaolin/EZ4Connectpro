package auth

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestEZ4DeviceMutationPayload(t *testing.T) {
	for _, action := range []string{"trust", "untrust", "replace"} {
		for _, denied := range []bool{false, true} {
			calls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls++
				path := "/passport/v1/security/trustDevice"
				if action == "untrust" {
					path = "/passport/v1/security/untrustDevice"
				}
				var payload map[string][]string
				if r.Method != "POST" || r.URL.Path != path || r.Header.Get("x-csrf-token") != "test-token" {
					t.Error("wrong mutation request")
				}
				key := "idList"
				if action == "replace" {
					key = "untrustIdList"
				}
				if json.NewDecoder(r.Body).Decode(&payload) != nil || len(payload) != 1 || len(payload[key]) != 1 || payload[key][0] != "selected-device" {
					t.Error("mutation targeted unexpected records")
				}
				if denied {
					fmt.Fprint(w, `{"code":75500311,"message":"Failed"}`)
				} else {
					fmt.Fprint(w, `{"code":0}`)
				}
			}))
			session := &Session{baseURL: server.URL, client: server.Client(), csrfToken: "test-token"}
			var err error
			if action == "untrust" {
				err = session.UntrustDevice([]string{"selected-device"})
			} else {
				err = session.ChangeTrustedDevice("selected-device", action == "replace")
			}
			server.Close()
			if (err != nil) != denied || calls != 1 {
				t.Fatalf("mutation error handling: err=%v calls=%d", err, calls)
			}
		}
	}
}

func TestEZ4ManagedTrustReplies(t *testing.T) {
	for _, body := range []string{`{"data":{}}`, `<html>expired</html>`, `{"code":0,"data":{"nextService":"auth/sms"}}`, `{"code":73700001}`, strings.Repeat(" ", 2*1024*1024+1)} {
		calls := 0
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls++; fmt.Fprint(w, body) }))
		session := &Session{baseURL: server.URL, client: server.Client(), csrfToken: "test-token"}
		err := session.ChangeTrustedDevice("selected-device", true)
		server.Close()
		if err == nil || calls != 1 {
			t.Fatal("pending, denied or malformed response treated as completed", err, calls)
		}
	}
}

func TestEZ4DeviceQuery(t *testing.T) {
	for _, test := range []struct {
		name, body string
		status     int
		success    bool
	}{
		{"valid", `{"code":0,"data":{"selfId":"self","deviceTrusted":false,"data":[]}}`, 200, true},
		{"denied", `{"code":75500311,"message":"Failed"}`, 200, false},
		{"missing code", `{"data":{}}`, 200, false},
		{"malformed", `<html>login</html>`, 200, false},
		{"forbidden", `{}`, 403, false},
		{"oversize", strings.Repeat(" ", 2*1024*1024+1), 200, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			calls := 0
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls++
				if r.Method != "GET" || r.URL.Path != "/passport/v1/security/queryDevice" || r.URL.Query().Get("status") != "trust" || r.Header.Get("x-csrf-token") != "test-token" {
					t.Error("incorrect query request")
				}
				w.WriteHeader(test.status)
				fmt.Fprint(w, test.body)
			}))
			defer server.Close()
			session := &Session{baseURL: server.URL, client: server.Client(), csrfToken: "test-token"}
			_, err := session.TrustedDeviceData()
			if (err == nil) != test.success || calls != 1 {
				t.Fatalf("unexpected query result %v, requests=%d", err, calls)
			}
		})
	}
}
