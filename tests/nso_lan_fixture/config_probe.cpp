// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/internal_network/nso_lan.h"
#include <iostream>
#include <fstream>
#include <iterator>
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
    if (mode == "automatic" || mode == "automatic-invalid" || mode == "automatic-conflict") {
        Check(argc == 3, "automatic input missing");
        std::ifstream input{argv[2]};
        Check(input.good(), "automatic input unreadable");
        const std::string text{std::istreambuf_iterator<char>{input}, {}};
        // Exercise publication after a prior caller has cached the disabled legacy state.
        const bool requested_before = NsoLan::IsRequested();
        const bool installed = NsoLan::InstallAutomaticConfiguration(
            text, "/run/user/1000/eden-lan/i-test");
        if (mode != "automatic") {
            Check(!installed, "invalid/conflicting automatic configuration accepted");
            Check(NsoLan::IsRequested() == requested_before, "failed publication changed request state");
            Check(!NsoLan::GetAutomaticTransport(), "failed publication exposed transport");
        } else {
            Check(!requested_before && installed && NsoLan::IsRequested(), "automatic publication failed");
            const auto* transport = NsoLan::GetAutomaticTransport();
            Check(transport && transport->instance_address == IPv4Address{10,253,1,2}, "automatic identity");
            Check(NsoLan::GetConfiguration()->account_id == 49153, "automatic account");
            const SockAddrIn auth{.family=Domain::INET, .ip={10,254,0,1}, .portno=443};
            const auto tcp = NsoLan::RouteDestination(auth, false);
            Check(tcp && tcp->ip == IPv4Address{127,0,0,1} && tcp->portno == 38443,
                  "automatic TLS must use local proxy");
            const auto udp = NsoLan::RouteDestination(auth, true);
            Check(udp && udp->ip == auth.ip && udp->portno == auth.portno,
                  "automatic UDP must retain logical endpoint");
            const SockAddrIn peer{.family=Domain::INET, .ip={10,253,7,8}, .portno=30123};
            const auto routed_peer = NsoLan::RouteDestination(peer, true);
            Check(routed_peer && routed_peer->ip == peer.ip && routed_peer->portno == peer.portno,
                  "dynamic peer must reach broker");
            Check(!NsoLan::RouteDestination(peer, false), "virtual peer escaped over native TCP");
            Check(!NsoLan::RouteDestination({.family=Domain::INET, .ip={10,254,0,1}, .portno=80}, false),
                  "unmapped service port escaped to native TCP");
            Check(!NsoLan::RouteDestination({.family=Domain::INET, .ip={10,254,0,2}, .portno=443}, false),
                  "unmapped service escaped to native TCP");
            Check(!NsoLan::RouteDestination({.family=Domain::INET, .ip={127,0,0,1}, .portno=38443}, false),
                  "direct guest loopback connection admitted");
            Check(!NsoLan::RouteDestination(public_ip, true), "automatic public UDP escaped");
            Check(!NsoLan::RouteDestination(local, true), "unregistered physical LAN UDP escaped");
            Check(NsoLan::ResolveHost(service_resolved) == "10.254.0.1", "automatic service DNS");
            Check(!NsoLan::ResolveHost("unknown.nintendo.net"), "automatic unknown DNS escaped");
            Check(!NsoLan::InstallAutomaticConfiguration(text, "/run/user/1000/eden-lan/i-test"),
                  "configuration was replaced after publication");
            Check(NsoLan::GetAutomaticTransport() == transport, "published transport pointer invalidated");
        }
    } else if (mode == "disabled") {
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
