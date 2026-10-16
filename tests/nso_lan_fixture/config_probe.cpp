// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/internal_network/nso_lan.h"
#include <iostream>
#include <stdexcept>

using namespace Network;
void Check(bool value, const char* label) {
    if (!value) throw std::runtime_error(label);
}
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const SockAddrIn local{.family=Domain::INET, .ip={192,168,10,20}, .portno=443};
    const SockAddrIn public_ip{.family=Domain::INET, .ip={8,8,8,8}, .portno=443};
    const std::string service_template = "g23380901-%.s.n.srv.nintendo.net";
    const std::string service_resolved = "g23380901-lp1.s.n.srv.nintendo.net";
    if (mode == "disabled") {
        Check(!NsoLan::IsRequested(), "disabled requested");
        const auto routed = NsoLan::RouteDestination(public_ip);
        Check(routed.has_value(), "disabled behavior changed");
        Check(routed->family == public_ip.family && routed->ip == public_ip.ip &&
              routed->portno == public_ip.portno, "disabled destination changed");
        Check(NsoLan::ResolveServiceName(service_template) == service_template,
              "disabled NSD substitution");
        Check(NsoLan::ResolveServiceName("AUTH.FIXTURE.TEST.") == "AUTH.FIXTURE.TEST.",
              "disabled ordinary NSD name changed");
    } else if (mode == "invalid") {
        Check(NsoLan::IsRequested(), "invalid not requested");
        Check(!NsoLan::GetConfiguration(), "invalid configuration accepted");
        Check(!NsoLan::ResolveHost("auth.fixture.test"), "invalid DNS leaked");
        Check(!NsoLan::RouteDestination(local), "invalid destination leaked");
        Check(NsoLan::ResolveServiceName(service_template) == service_template,
              "invalid configuration NSD substitution");
    } else if (mode == "valid") {
        Check(NsoLan::GetConfiguration().has_value(), "valid configuration rejected");
        Check(NsoLan::GetConfiguration()->account_id == 49153, "identity mismatch");
        Check(NsoLan::ResolveServiceName(service_template) == service_resolved,
              "observed NSD environment template");
        Check(NsoLan::ResolveHost(NsoLan::ResolveServiceName(service_template)) == "192.168.10.20",
              "resolved NSD name not admitted by fixture DNS");
        Check(NsoLan::ResolveServiceName("G23380901-%.S.N.SRV.NINTENDO.NET.") ==
              "G23380901-lp1.S.N.SRV.NINTENDO.NET.", "NSD normalized mapping lookup");
        const std::string unmapped_template = "g23380902-%.s.n.srv.nintendo.net";
        Check(NsoLan::ResolveServiceName(unmapped_template) == unmapped_template,
              "unmapped NSD template broadened");
        Check(!NsoLan::ResolveHost(NsoLan::ResolveServiceName(unmapped_template)),
              "unmapped NSD template admitted by DNS");
        Check(NsoLan::ResolveServiceName(service_template + ".evil") == service_template + ".evil",
              "NSD suffix confusion");
        Check(NsoLan::ResolveServiceName("AUTH.FIXTURE.TEST.") == "AUTH.FIXTURE.TEST.",
              "ordinary mapped NSD name changed");
        Check(NsoLan::ResolveServiceName(service_resolved) == service_resolved,
              "already resolved NSD name changed");
        Check(NsoLan::ResolveServiceName("g23380901-%s.s.n.srv.nintendo.net") ==
              "g23380901-%s.s.n.srv.nintendo.net", "unsupported NSD syntax changed");
        Check(NsoLan::ResolveHost("AUTH.FIXTURE.TEST.") == "192.168.10.20", "mapped DNS");
        Check(!NsoLan::ResolveHost("auth.fixture.test.evil"), "suffix confusion");
        Check(!NsoLan::ResolveHost("unknown.nintendo.net"), "unmapped DNS fallback");
        Check(!NsoLan::ResolveHost("8.8.8.8"), "literal public IP");
        Check(!NsoLan::RouteDestination(public_ip), "public destination");
        const auto routed = NsoLan::RouteDestination(local);
        Check(routed.has_value() && routed->portno == 8443, "port mapping");
        auto peer = local;
        peer.ip = {192,168,10,21}; peer.portno = 30000;
        const auto routed_peer = NsoLan::RouteDestination(peer);
        Check(routed_peer.has_value() && routed_peer->portno == 30000, "peer traffic altered");
        peer.ip = {192,168,10,22};
        Check(!NsoLan::RouteDestination(peer), "unlisted LAN peer");
    } else throw std::runtime_error("unknown mode");
    std::cout << mode << " PASS\n";
}
