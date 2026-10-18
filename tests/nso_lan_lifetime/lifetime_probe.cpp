// SPDX-License-Identifier: GPL-3.0-or-later
// Regression probe for guest socket descriptor lifetime in the bsd service (review item F1).
//
// Every fixed-path case drives the production Service::Sockets::DescriptorTable that bsd.cpp
// uses. The socket is a probe SocketBase over a real AF_UNIX datagram socketpair: Recv blocks in
// the kernel and Close wakes it with shutdown(SHUT_RDWR), like NsoLanSocket::CloseLocked. The
// receive and close sequences follow BSD_USA::RecvImpl and BSD_USA::CloseImpl.
//
// `--legacy-repro` replays the pre-fix bsd.cpp pattern (a FileDescriptor& into the table across
// the blocking receive, then Close and reset of the last reference). It is expected to fail
// under AddressSanitizer with heap-use-after-free and is not part of the passing suite.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include "core/hle/service/sockets/descriptor_table.h"
#include "core/internal_network/sockets.h"

namespace {

using Service::Sockets::DescriptorTable;
using Clock = std::chrono::steady_clock;

constexpr u32 kAlive = 0x51DEC0DEu;
constexpr u32 kDead = 0xDEADDEADu;
constexpr auto kWakeBound = std::chrono::milliseconds{750};
constexpr int kReceiveTimeoutMs = 2000;

std::atomic<int> g_destroyed{0};

// Gate used to hold a woken receiver until the closer has dropped every other reference, which
// maximizes the window in which the pre-fix pattern dereferences a destroyed socket. It is global
// so that waiting on it never touches the socket object itself.
std::atomic<bool> g_gate_enabled{false};
std::atomic<bool> g_gate_open{false};
std::atomic<Clock::rep> g_recv_returned_at{0};

void WaitGate() {
    if (!g_gate_enabled.load()) {
        return;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds{kReceiveTimeoutMs};
    while (!g_gate_open.load() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
}

class ProbeSocket final : public Network::SocketBase {
public:
    ProbeSocket() {
        int pair[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, pair) != 0) {
            std::perror("socketpair");
            std::abort();
        }
        fd = pair[0];
        peer = pair[1];
        timeval timeout{.tv_sec = kReceiveTimeoutMs / 1000,
                        .tv_usec = (kReceiveTimeoutMs % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    ~ProbeSocket() override {
        canary = kDead;
        if (fd != INVALID_SOCKET) {
            ::close(fd);
        }
        if (peer >= 0) {
            ::close(peer);
        }
        g_destroyed.fetch_add(1);
    }

    std::atomic<pid_t> receiver_tid{0};
    std::atomic<int> nonblock_calls{0};
    std::atomic<int> proxy_packets{0};
    u32 canary = kAlive;
    ssize_t last_received = -2;

    Network::Errno Initialize(Network::Domain, Network::Type, Network::Protocol) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno Close() override {
        std::scoped_lock lock{mutex};
        if (fd != INVALID_SOCKET) {
            (void)::shutdown(fd, SHUT_RDWR); // wakes a receiver blocked in recv
            ::close(fd);
            fd = INVALID_SOCKET;
        }
        return Network::Errno::SUCCESS;
    }

    std::pair<s32, Network::Errno> Recv(int, std::span<u8> message) override {
        receiver_tid.store(static_cast<pid_t>(::syscall(SYS_gettid)));
        int local_fd;
        {
            std::scoped_lock lock{mutex};
            local_fd = fd;
        }
        const ssize_t n = ::recv(local_fd, message.data(), message.size(), 0);
        const int error = errno;
        g_recv_returned_at.store(Clock::now().time_since_epoch().count());
        WaitGate();
        // Member accesses after the wake: a use-after-free if the socket was destroyed.
        last_received = n;
        if (canary != kAlive) {
            std::fprintf(stderr, "canary violated after wake\n");
        }
        if (n < 0) {
            return {-1, error == EAGAIN || error == EWOULDBLOCK ? Network::Errno::AGAIN
                                                                 : Network::Errno::OTHER};
        }
        return {static_cast<s32>(n), Network::Errno::SUCCESS};
    }

    Network::Errno SetNonBlock(bool) override {
        nonblock_calls.fetch_add(1);
        if (canary != kAlive) {
            std::fprintf(stderr, "canary violated in SetNonBlock\n");
        }
        return Network::Errno::SUCCESS;
    }

    bool IsOpened() const override {
        std::scoped_lock lock{mutex};
        return canary == kAlive && fd != INVALID_SOCKET;
    }

    void HandleProxyPacket(const Network::ProxyPacket&) override {
        proxy_packets.fetch_add(1);
    }

    // Unused by the probe.
    std::pair<AcceptResult, Network::Errno> Accept() override {
        return {AcceptResult{}, Network::Errno::INVAL};
    }
    Network::Errno Connect(Network::SockAddrIn) override { return Network::Errno::INVAL; }
    std::pair<Network::SockAddrIn, Network::Errno> GetPeerName() override {
        return {Network::SockAddrIn{}, Network::Errno::INVAL};
    }
    std::pair<Network::SockAddrIn, Network::Errno> GetSockName() override {
        return {Network::SockAddrIn{}, Network::Errno::INVAL};
    }
    Network::Errno Bind(Network::SockAddrIn) override { return Network::Errno::INVAL; }
    Network::Errno Listen(s32) override { return Network::Errno::INVAL; }
    Network::Errno Shutdown(Network::ShutdownHow) override { return Network::Errno::INVAL; }
    std::pair<s32, Network::Errno> RecvFrom(int, std::span<u8>, Network::SockAddrIn*) override {
        return {-1, Network::Errno::INVAL};
    }
    std::pair<s32, Network::Errno> Send(std::span<const u8>, int) override {
        return {-1, Network::Errno::INVAL};
    }
    std::pair<s32, Network::Errno> SendTo(u32, std::span<const u8>,
                                          const Network::SockAddrIn*) override {
        return {-1, Network::Errno::INVAL};
    }
    Network::Errno SetLinger(bool, u32) override { return Network::Errno::INVAL; }
    Network::Errno SetReuseAddr(bool) override { return Network::Errno::INVAL; }
    Network::Errno SetKeepAlive(bool) override { return Network::Errno::INVAL; }
    Network::Errno SetBroadcast(bool) override { return Network::Errno::INVAL; }
    Network::Errno SetSndBuf(u32) override { return Network::Errno::INVAL; }
    Network::Errno SetRcvBuf(u32) override { return Network::Errno::INVAL; }
    Network::Errno SetSndTimeo(u32) override { return Network::Errno::INVAL; }
    Network::Errno SetRcvTimeo(u32) override { return Network::Errno::INVAL; }
    std::pair<Network::Errno, Network::Errno> GetPendingError() override {
        return {Network::Errno::SUCCESS, Network::Errno::SUCCESS};
    }

private:
    mutable std::mutex mutex;
    int peer = -1;
};

// ---- minimal harness -------------------------------------------------------------------------

int g_checks_failed_in_test = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_checks_failed_in_test;                                                             \
            std::fprintf(stderr, "  check failed at line %d: %s\n", __LINE__, #cond);              \
        }                                                                                          \
    } while (false)

/// Waits until `tid` is inside the recvfrom syscall (glibc recv), as reported by the kernel.
bool WaitBlockedInRecv(pid_t tid) {
    const std::string path = "/proc/self/task/" + std::to_string(tid) + "/syscall";
    const auto deadline = Clock::now() + std::chrono::milliseconds{kReceiveTimeoutMs / 2};
    while (Clock::now() < deadline) {
        std::ifstream in{path};
        long number = -1;
        if (in >> number && number == SYS_recvfrom) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}

pid_t WaitTid(const std::shared_ptr<ProbeSocket>& socket) {
    const auto deadline = Clock::now() + std::chrono::milliseconds{kReceiveTimeoutMs / 2};
    while (socket->receiver_tid.load() == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return socket->receiver_tid.load();
}

void ResetGate(bool enabled) {
    g_gate_open = false;
    g_gate_enabled = enabled;
    g_recv_returned_at = 0;
}

std::shared_ptr<Network::SocketBase> SocketAt(const DescriptorTable& table, s32 fd) {
    const auto entry = table.Get(fd);
    return entry ? entry->socket : nullptr;
}

u64 SerialAt(const DescriptorTable& table, s32 fd) {
    const auto entry = table.Get(fd);
    return entry ? entry->serial : 0;
}

// BSD_USA::CloseImpl: snapshot, Close outside the table lock, then erase only that publication.
bool CloseLikeBsd(DescriptorTable& table, s32 fd) {
    auto descriptor = table.Get(fd);
    if (!descriptor) {
        return false;
    }
    if (descriptor->socket->Close() != Network::Errno::SUCCESS) {
        return false;
    }
    return table.Erase(fd, descriptor->serial);
}

struct ReceiveOutcome {
    s32 ret = -99;
    Network::Errno error = Network::Errno::OTHER;
    int destroyed_after_return = -1;
    bool restore_skipped = false;
    bool opened_after_return = true;
    int destroyed_before_drop = -1;
    int destroyed_after_drop = -1;
};

// BSD_USA::RecvImpl with a blocking descriptor: snapshot, receive without the table lock, then
// restore the blocking mode only if the same publication is still current.
ReceiveOutcome ReceiveLikeBsd(const DescriptorTable& table, s32 fd) {
    ReceiveOutcome outcome;
    auto descriptor = table.Get(fd);
    if (!descriptor) {
        outcome.ret = -1;
        outcome.error = Network::Errno::BADF;
        return outcome;
    }
    std::array<u8, 64> buffer{};
    const auto [ret, error] = descriptor->socket->Recv(0, buffer);
    outcome.ret = ret;
    outcome.error = error;
    outcome.destroyed_after_return = g_destroyed.load();
    const auto current_flags = table.GetFlags(fd, descriptor->serial);
    outcome.restore_skipped = !current_flags.has_value();
    if (current_flags && (*current_flags & Network::FLAG_O_NONBLOCK) == 0) {
        descriptor->socket->SetNonBlock(false);
    }
    // Use the socket after the receive returned; the snapshot must still own it.
    outcome.opened_after_return = descriptor->socket->IsOpened();
    outcome.destroyed_before_drop = g_destroyed.load();
    descriptor.reset(); // the receiver is now the last owner
    outcome.destroyed_after_drop = g_destroyed.load();
    return outcome;
}

// ---- cases -----------------------------------------------------------------------------------

/// A receive blocked in the kernel survives a concurrent Close that removes the table entry and
/// drops every other reference; Close wakes it and the receiver performs the last-owner drop.
void BlockedReceiveSurvivesCloseAndLastOwnerDrop(bool reuse_descriptor) {
    ResetGate(true);
    const int destroyed_at_start = g_destroyed.load();
    DescriptorTable table;
    auto socket = std::make_shared<ProbeSocket>();
    std::weak_ptr<ProbeSocket> weak = socket;
    const s32 fd = table.Insert(DescriptorTable::Entry{.socket = socket});
    CHECK(fd == 0);

    auto receiver = std::async(std::launch::async, [&table, fd] { return ReceiveLikeBsd(table, fd); });
    const pid_t tid = WaitTid(socket);
    socket.reset(); // the table and the receiver's snapshot are now the only owners
    CHECK(tid != 0);
    CHECK(WaitBlockedInRecv(tid));

    const auto close_at = Clock::now();
    CHECK(CloseLikeBsd(table, fd));
    CHECK(!table.Get(fd).has_value());

    std::shared_ptr<ProbeSocket> replacement;
    std::optional<DescriptorTable::Entry> replacement_entry;
    if (reuse_descriptor) {
        // Another guest thread is given the same number while the receiver is still inside Recv.
        replacement = std::make_shared<ProbeSocket>();
        CHECK(table.Insert(DescriptorTable::Entry{.socket = replacement, .flags = 0}) == fd);
        replacement_entry = table.Get(fd);
    }

    CHECK(!weak.expired());                          // the receiver's snapshot still owns it
    CHECK(g_destroyed.load() == destroyed_at_start); // nothing destroyed before the receiver ran
    g_gate_open = true;

    if (receiver.wait_for(std::chrono::milliseconds{kReceiveTimeoutMs * 2}) !=
        std::future_status::ready) {
        std::fprintf(stderr, "receiver did not return; aborting\n");
        std::_Exit(3);
    }
    const ReceiveOutcome outcome = receiver.get();
    const auto woke_after =
        Clock::time_point{Clock::duration{g_recv_returned_at.load()}} - close_at;

    CHECK(outcome.ret == 0); // EOF from shutdown, not the AGAIN of the receive timeout
    CHECK(outcome.error == Network::Errno::SUCCESS);
    CHECK(woke_after < kWakeBound);
    CHECK(outcome.destroyed_after_return == destroyed_at_start);
    CHECK(outcome.restore_skipped);
    CHECK(!outcome.opened_after_return); // closed, but still a live object
    CHECK(outcome.destroyed_before_drop == destroyed_at_start);
    CHECK(outcome.destroyed_after_drop == destroyed_at_start + 1);
    CHECK(weak.expired());

    if (reuse_descriptor) {
        const auto current = table.Get(fd);
        CHECK(current.has_value() && current->socket == replacement);
        CHECK(current.has_value() && replacement_entry.has_value() &&
              current->serial == replacement_entry->serial);
        CHECK(replacement->nonblock_calls.load() == 0); // the old receiver never touched it
        CHECK(replacement->IsOpened());
    }
    ResetGate(false);
}

/// A Close that loses a race with another Close cannot erase the descriptor that reused fd, and
/// a stale Fcntl publication cannot change its flags.
void StaleCloseAndFlagsDoNotTouchReplacement() {
    DescriptorTable table;
    auto first = std::make_shared<ProbeSocket>();
    const s32 fd = table.Insert(DescriptorTable::Entry{.socket = first});
    const auto stale = table.Get(fd); // closer A takes its snapshot, then is preempted
    CHECK(stale.has_value());

    CHECK(CloseLikeBsd(table, fd)); // closer B completes
    auto second = std::make_shared<ProbeSocket>();
    const s32 reused = table.Insert(DescriptorTable::Entry{.socket = second, .flags = 0});
    CHECK(reused == fd);
    const auto current = table.Get(fd);
    CHECK(current.has_value() && current->serial != stale->serial);

    // Closer A resumes with its stale snapshot.
    CHECK(stale->socket->Close() == Network::Errno::SUCCESS);
    CHECK(!table.Erase(fd, stale->serial));
    CHECK(!table.SetFlags(fd, stale->serial, static_cast<s32>(Network::FLAG_O_NONBLOCK)));
    CHECK(!table.GetFlags(fd, stale->serial).has_value());

    const auto after = table.Get(fd);
    CHECK(after.has_value() && after->socket == second && after->flags == 0);
    CHECK(table.GetFlags(fd, current->serial) == std::optional<s32>{0});
    CHECK(second->IsOpened());

    // The current publication can still be updated and closed normally.
    CHECK(table.SetFlags(fd, current->serial, static_cast<s32>(Network::FLAG_O_NONBLOCK)));
    CHECK(table.GetFlags(fd, current->serial) ==
          std::optional<s32>{static_cast<s32>(Network::FLAG_O_NONBLOCK)});
    CHECK(CloseLikeBsd(table, fd));
    CHECK(!table.Get(fd).has_value());
}

/// Descriptor numbers are 0..127; 128 and negatives are refused everywhere.
void Descriptor128IsRefused() {
    CHECK(!DescriptorTable::InRange(-1));
    CHECK(DescriptorTable::InRange(0));
    CHECK(DescriptorTable::InRange(127));
    CHECK(!DescriptorTable::InRange(128));
    CHECK(!DescriptorTable::InRange(INT_MAX));
    CHECK(!DescriptorTable::InRange(INT_MIN));

    DescriptorTable table;
    std::vector<s32> fds;
    for (int i = 0; i < 128; ++i) {
        fds.push_back(table.Insert(DescriptorTable::Entry{.socket = std::make_shared<ProbeSocket>()}));
    }
    CHECK(fds.front() == 0 && fds.back() == 127);
    CHECK(!table.HasFree());
    CHECK(table.Insert(DescriptorTable::Entry{.socket = std::make_shared<ProbeSocket>()}) == -1);
    CHECK(table.Get(127).has_value());
    CHECK(!table.Get(128).has_value());
    CHECK(!table.Get(-1).has_value());
    const u64 last_serial = SerialAt(table, 127);
    CHECK(last_serial != 0);
    CHECK(!table.Erase(128, last_serial));
    CHECK(!table.Erase(128, last_serial + 1));
    CHECK(!table.SetFlags(128, last_serial, 1));
    CHECK(!table.GetFlags(128, last_serial).has_value());
    CHECK(table.Sockets().size() == 128);
}

/// Allocation after a blocking Accept is atomic: the last slot goes to exactly one publisher and
/// a publisher that loses gets -1 without overwriting the winner or leaving a reservation.
void AcceptAllocationIsAtomic() {
    DescriptorTable table;
    for (int i = 0; i < 127; ++i) {
        (void)table.Insert(DescriptorTable::Entry{.socket = std::make_shared<ProbeSocket>()});
    }
    CHECK(table.HasFree()); // AcceptImpl's pre-check passes ...

    // ... and another thread takes the last slot while Accept blocks.
    auto other = std::make_shared<ProbeSocket>();
    CHECK(table.Insert(DescriptorTable::Entry{.socket = other}) == 127);
    auto accepted = std::make_shared<ProbeSocket>();
    std::weak_ptr<ProbeSocket> weak_accepted = accepted;
    CHECK(table.Insert(DescriptorTable::Entry{.socket = std::move(accepted),
                                              .is_connection_based = true}) == -1);
    CHECK(weak_accepted.expired()); // the refused socket is not retained anywhere
    CHECK(SocketAt(table, 127) == other);
    CHECK(!table.HasFree());

    // Two publishers race for one freed slot.
    CHECK(CloseLikeBsd(table, 127));
    for (int round = 0; round < 50; ++round) {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::array<std::shared_ptr<ProbeSocket>, 2> sockets{std::make_shared<ProbeSocket>(),
                                                            std::make_shared<ProbeSocket>()};
        std::array<s32, 2> results{-2, -2};
        std::array<std::thread, 2> threads;
        for (int t = 0; t < 2; ++t) {
            threads[t] = std::thread([&, t] {
                ++ready;
                while (!go.load()) {
                    std::this_thread::yield();
                }
                results[t] = table.Insert(DescriptorTable::Entry{.socket = sockets[t]});
            });
        }
        while (ready.load() != 2) {
            std::this_thread::yield();
        }
        go = true;
        for (auto& thread : threads) {
            thread.join();
        }
        const int winners = (results[0] == 127) + (results[1] == 127);
        const int losers = (results[0] == -1) + (results[1] == -1);
        CHECK(winners == 1 && losers == 1);
        const auto entry = table.Get(127);
        CHECK(entry.has_value() && entry->socket == sockets[results[0] == 127 ? 0 : 1]);
        CHECK(CloseLikeBsd(table, 127));
        if (g_checks_failed_in_test != 0) {
            break;
        }
    }
}

/// The LDN proxy callback runs on snapshots: entries removed (and sockets released by the table)
/// during delivery stay alive until delivery finishes, and the table is usable re-entrantly.
void ProxyCallbackSnapshotOutlivesClose() {
    const int destroyed_at_start = g_destroyed.load();
    DescriptorTable table;
    auto a = std::make_shared<ProbeSocket>();
    auto b = std::make_shared<ProbeSocket>();
    const s32 fd_a = table.Insert(DescriptorTable::Entry{.socket = a});
    const s32 fd_b = table.Insert(DescriptorTable::Entry{.socket = b});
    const s32 fd_dup = table.Insert(*table.Get(fd_a)); // DuplicateSocketImpl
    CHECK(fd_a == 0 && fd_b == 1 && fd_dup == 2);
    CHECK(SerialAt(table, fd_dup) != 0 && SerialAt(table, fd_dup) != SerialAt(table, fd_a));
    CHECK(SocketAt(table, fd_dup) == SocketAt(table, fd_a));
    std::weak_ptr<ProbeSocket> weak_a = a;
    std::weak_ptr<ProbeSocket> weak_b = b;
    a.reset();
    b.reset();

    std::vector<Network::SocketBase*> order;
    {
        const auto sockets = table.Sockets();
        CHECK(sockets.size() == 3);
        for (const auto& socket : sockets) {
            // Re-entrant table use inside delivery: would deadlock if the lock were held.
            for (const s32 fd : {fd_a, fd_b, fd_dup}) {
                if (const auto entry = table.Get(fd)) {
                    (void)table.Erase(fd, entry->serial);
                }
            }
            CHECK(g_destroyed.load() == destroyed_at_start);
            CHECK(socket->IsOpened()); // stands in for HandleProxyPacket on a live object
            order.push_back(socket.get());
        }
        CHECK(!weak_a.expired() && !weak_b.expired());
    }
    CHECK(order.size() == 3 && order[0] == order[2] && order[0] != order[1]);
    CHECK(weak_a.expired() && weak_b.expired());
    CHECK(g_destroyed.load() == destroyed_at_start + 2);
    CHECK(table.Sockets().empty());
}

/// Concurrent churn on one table: every thread's own publication can be updated and erased by
/// serial, a stale serial never succeeds, and the table ends empty.
void ConcurrentChurnKeepsIdentity() {
    DescriptorTable table;
    constexpr int kThreads = 4;
    constexpr int kIterations = 2000;
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                const s32 fd = table.Insert(DescriptorTable::Entry{.socket = std::make_shared<ProbeSocket>()});
                if (fd < 0) {
                    ++failures;
                    continue;
                }
                const auto entry = table.Get(fd);
                if (!entry || !table.SetFlags(fd, entry->serial, i) ||
                    table.GetFlags(fd, entry->serial) != std::optional<s32>{i} ||
                    table.Erase(fd, entry->serial + 1'000'000'000ull) ||
                    !table.Erase(fd, entry->serial) || table.Erase(fd, entry->serial) ||
                    table.SetFlags(fd, entry->serial, 0)) {
                    ++failures;
                }
            }
        });
    }
    std::thread observer([&] {
        while (!stop.load()) {
            for (const auto& socket : table.Sockets()) {
                if (!socket || !socket->IsOpened()) {
                    ++failures;
                }
            }
            std::this_thread::yield();
        }
    });
    for (auto& thread : threads) {
        thread.join();
    }
    stop = true;
    observer.join();
    CHECK(failures.load() == 0);
    CHECK(table.Sockets().empty());
    CHECK(table.HasFree());
}

// ---- pre-fix reproduction --------------------------------------------------------------------

// Mirrors the pre-fix bsd.cpp (sha256 f5dce224...aaca): RecvImpl held `FileDescriptor&` into the
// static table across the blocking Recv and then called SetNonBlock; CloseImpl closed and reset
// the entry, dropping the last reference.
struct LegacyFileDescriptor {
    std::shared_ptr<Network::SocketBase> socket;
    s32 flags = 0;
    bool is_connection_based = false;
};
std::array<std::optional<LegacyFileDescriptor>, 128> g_legacy_table{};

int LegacyRepro() {
    ResetGate(true);
    auto socket = std::make_shared<ProbeSocket>();
    g_legacy_table[0] = LegacyFileDescriptor{.socket = socket};
    auto receiver = std::async(std::launch::async, [] {
        LegacyFileDescriptor& descriptor = *g_legacy_table[0];
        std::array<u8, 64> buffer{};
        const auto [ret, error] = descriptor.socket->Recv(0, buffer);
        if ((descriptor.flags & Network::FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(false);
        }
        return ret;
    });
    const pid_t tid = WaitTid(socket);
    socket.reset();
    if (tid == 0 || !WaitBlockedInRecv(tid)) {
        std::fprintf(stderr, "legacy: receiver never blocked\n");
        return 2;
    }
    (void)g_legacy_table[0]->socket->Close();
    g_legacy_table[0].reset(); // destroys the socket while the receiver is still inside Recv
    std::fprintf(stderr, "legacy: destroyed=%d before the receiver resumed\n", g_destroyed.load());
    g_gate_open = true;
    const s32 ret = receiver.get();
    std::fprintf(stderr, "legacy: receiver returned %d without a sanitizer report\n", ret);
    return 1;
}

struct Case {
    const char* name;
    void (*run)();
};

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == "--legacy-repro") {
        return LegacyRepro();
    }

    const std::array<Case, 7> cases{{
        {"BlockedReceiveSurvivesCloseAndLastOwnerDrop",
         [] { BlockedReceiveSurvivesCloseAndLastOwnerDrop(false); }},
        {"BlockedReceiveDoesNotTouchReusedDescriptor",
         [] { BlockedReceiveSurvivesCloseAndLastOwnerDrop(true); }},
        {"StaleCloseAndFlagsDoNotTouchReplacement", StaleCloseAndFlagsDoNotTouchReplacement},
        {"Descriptor128IsRefused", Descriptor128IsRefused},
        {"AcceptAllocationIsAtomic", AcceptAllocationIsAtomic},
        {"ProxyCallbackSnapshotOutlivesClose", ProxyCallbackSnapshotOutlivesClose},
        {"ConcurrentChurnKeepsIdentity", ConcurrentChurnKeepsIdentity},
    }};

    int passed = 0;
    int failed = 0;
    for (const auto& test_case : cases) {
        g_checks_failed_in_test = 0;
        test_case.run();
        if (g_checks_failed_in_test == 0) {
            ++passed;
            std::printf("[PASS] %s\n", test_case.name);
        } else {
            ++failed;
            std::printf("[FAIL] %s (%d checks)\n", test_case.name, g_checks_failed_in_test);
        }
    }
    std::printf("tests=%zu passed=%d failed=%d\n", cases.size(), passed, failed);
    return failed == 0 ? 0 : 1;
}
