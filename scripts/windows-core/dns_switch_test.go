//go:build windows

package tun

import (
	"net"
	"testing"

	"github.com/mythologyli/zju-connect/internal/zctcpip"
	"github.com/mythologyli/zju-connect/service"
)

func TestEZ4DNSInterceptionRequiresOptIn(t *testing.T) {
	ip := make(zctcpip.IPv4Packet, 20)
	ip.SetDestinationIP(net.ParseIP("192.0.2.53"))
	udp := make(zctcpip.UDPPacket, 8)
	udp.SetDestinationPort(53)
	s := &Stack{resolve: service.NewDnsServer(nil, []string{"192.0.2.54"})}
	if s.shouldHijackUDPDns(ip, udp) {
		t.Fatal("DNS interception occurred while the switch was off")
	}
	s.dnsHijack = true
	if !s.shouldHijackUDPDns(ip, udp) {
		t.Fatal("explicit DNS interception was not honored")
	}
	ip.SetDestinationIP(net.ParseIP("192.0.2.54"))
	if s.shouldHijackUDPDns(ip, udp) {
		t.Fatal("resolver upstream was intercepted, causing a lookup loop")
	}
	ip.SetDestinationIP(net.ParseIP("192.0.2.53"))
	udp.SetDestinationPort(443)
	if s.shouldHijackUDPDns(ip, udp) {
		t.Fatal("non-DNS traffic was intercepted")
	}
}
