// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/socket_types.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/nso_lan.h"
#ifdef EDEN_LAN_ONLINE_SOCKET
#include "core/internal_network/nso_lan_socket.h"
#endif
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"
#include <common/settings.h>

namespace Service::Sockets {

namespace {

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), (std::min)(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, (std::min)(sizeof(T), buffer.size()));
}

} // Anonymous namespace

void BSD_USA::PollWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->PollImpl(write_buffer, read_buffer, nfds, timeout);
}

void BSD_USA::PollWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::AcceptWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD_USA::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::ConnectWork::Execute(BSD_USA* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
}

void BSD_USA::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
}

void BSD_USA::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvFromWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr);
}

void BSD_USA::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(addr.size()));
}

void BSD_USA::SendWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
}

void BSD_USA::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SendToWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
}

void BSD_USA::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RegisterClient(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD_USA::StartMonitoring(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};

    rb.Push(ResultSuccess);
}

void BSD_USA::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SocketExempt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));
    if (bsd_errno == Errno::SUCCESS) {
        bsd_errno = ShutdownImpl(fd, 0);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Select(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // ret
    rb.Push<u32>(0); // bsd errno
}

void BSD_USA::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    ExecuteWork(ctx, PollWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_buffer = ctx.ReadBuffer(),
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD_USA::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD_USA::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} len={:#x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);

    ctx.WriteBuffer(optval);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD_USA::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD_USA::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 cmd = rp.Pop<s32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    BuildErrnoResponse(ctx, SetSockOptImpl(fd, level, optname, optval));
}

void BSD_USA::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD_USA::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD_USA::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD_USA::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD_USA::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = 0,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD_USA::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_WARNING(Service, "(STUBBED) called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // ret
    rb.Push<u32>(0); // bsd errno
}

void BSD_USA::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    BuildErrnoResponse(ctx, CloseImpl(fd));
}

/// @brief Only bsd:s is able to dup()
void BSD_USA::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    if (is_user) {
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = Errno::INVAL,
        });
        return;
    }

    auto const res_v = DuplicateSocketImpl(input.fd);
    if (auto* res = std::get_if<s32>(&res_v)) {
        rb.PushRaw(OutputParameters{
            .ret = *res,
            .bsd_errno = Errno::SUCCESS,
        });
    } else {
        auto* err = std::get_if<Errno>(&res_v);
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = *err,
        });
    }
}

void BSD_USA::EventFd(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u64 initval = rp.Pop<u64>();
    const u32 flags = rp.Pop<u32>();

    LOG_WARNING(Service, "(STUBBED) called. initval={}, flags={}", initval, flags);

    BuildErrnoResponse(ctx, Errno::SUCCESS);
}

template <typename Work>
void BSD_USA::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD_USA::SocketImpl(Domain domain, Type type, Protocol protocol) {
    // user bsd:u has restrictions on SOCK_SEQPACKET and SOCK_RAW
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW)) {
        if (type == Type::RAW && domain == Domain::INET && protocol == Protocol::ICMP) {
            // fine, can use on bsd:s and bsd:u
        } else {
            return {-1, Errno::INVAL};
        }
    }

    [[maybe_unused]] const bool unk_flag = (static_cast<u32>(type) & 0x20000000) != 0;
    UNIMPLEMENTED_IF_MSG(unk_flag, "Unknown flag in type");
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);

    if (!file_descriptors.HasFree()) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    // Create and initialize before publishing; initialization may block on the LAN runtime.
    DescriptorTable::Entry descriptor{};
    // ENONMEM might be thrown here

    auto room_member = Network::GetRoomMember().lock();
    const auto* automatic = Network::NsoLan::GetAutomaticTransport();
#ifdef EDEN_LAN_ONLINE_SOCKET
    if (automatic && type == Type::DGRAM) {
        descriptor.socket = std::make_shared<Network::NsoLanSocket>(
            automatic->control_socket, automatic->datagram_socket, automatic->instance_address);
    } else
#endif
    if (!automatic && room_member && room_member->IsConnected()) {
        descriptor.socket = std::make_shared<Network::ProxySocket>();
    } else {
        descriptor.socket = std::make_shared<Network::Socket>();
    }

    const auto initialize_result =
        descriptor.socket->Initialize(Translate(domain), Translate(type), Translate(protocol));
    if (automatic && initialize_result != Network::Errno::SUCCESS) {
        return {-1, Translate(initialize_result)};
    }
    descriptor.is_connection_based = IsConnectionBased(type);

    const bool is_connection_based = descriptor.is_connection_based;
    const s32 fd = file_descriptors.Insert(std::move(descriptor));
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    LOG_INFO(Service, "New socket fd={}", fd);

    if (Settings::values.airplane_mode.GetValue() && is_connection_based) {
        LOG_ERROR(Service, "Airplane mode is enabled, cannot create socket");
        return {-1, Errno::NOTCONN};
    }

    return {fd, Errno::SUCCESS};
}

std::pair<s32, Errno> BSD_USA::PollImpl(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds, s32 timeout) {
    if (nfds <= 0) {
        // When no entries are provided, -1 is returned with errno zero
        return {-1, Errno::SUCCESS};
    }
    if (read_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }
    if (write_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }

    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    if (timeout >= 0) {
        const s64 seconds = timeout / 1000;
        const u64 nanoseconds = 1'000'000 * (static_cast<u64>(timeout) % 1000);

        if (seconds < 0) {
            return {-1, Errno::INVAL};
        }
        if (nanoseconds > 999'999'999) {
            return {-1, Errno::INVAL};
        }
    } else if (timeout != -1) {
        return {-1, Errno::INVAL};
    }

    // Strong references keep every polled socket alive while Network::Poll blocks.
    std::vector<std::shared_ptr<Network::SocketBase>> sockets;
    sockets.reserve(fds.size());
    for (PollFD& pollfd : fds) {
        ASSERT(False(pollfd.revents));

        if (!DescriptorTable::InRange(pollfd.fd)) {
            LOG_ERROR(Service, "File descriptor handle={} is invalid", pollfd.fd);
            pollfd.revents = PollEvents{};
            return {0, Errno::SUCCESS};
        }

        auto descriptor = file_descriptors.Get(pollfd.fd);
        if (!descriptor) {
            LOG_TRACE(Service, "File descriptor handle={} is not allocated", pollfd.fd);
            pollfd.revents = PollEvents::Nval;
            return {0, Errno::SUCCESS};
        }
        sockets.push_back(std::move(descriptor->socket));
    }

    std::vector<Network::PollFD> host_pollfds(fds.size());
    std::transform(fds.begin(), fds.end(), sockets.begin(), host_pollfds.begin(),
                   [](PollFD pollfd, const std::shared_ptr<Network::SocketBase>& socket) {
                       Network::PollFD result;
                       result.socket = socket.get();
                       result.events = Translate(pollfd.events);
                       result.revents = Network::PollEvents{};
                       return result;
                   });

    const auto result = Network::Poll(host_pollfds, timeout);

    const size_t num = host_pollfds.size();
    for (size_t i = 0; i < num; ++i) {
        fds[i].revents = Translate(host_pollfds[i].revents);
    }
    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));

    return Translate(result);
}

std::pair<s32, Errno> BSD_USA::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    if (!file_descriptors.HasFree()) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    auto [result, bsd_errno] = descriptor->socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    if (!Network::NsoLan::RouteDestination(result.sockaddr_in)) {
        // Reject before publishing the accepted socket to the guest. The temporary socket
        // owns and closes its descriptor on this path, including invalid fixture configs.
        return {-1, Errno::CONNREFUSED};
    }

    // Allocate only now: a slot reserved before the blocking Accept could have been reused.
    const s32 new_fd = file_descriptors.Insert(DescriptorTable::Entry{
        .socket = std::move(result.socket),
        .is_connection_based = descriptor->is_connection_based,
    });
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    PutValue(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD_USA::BindImpl(s32 fd, std::span<const u8> addr) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    ASSERT(addr.size() >= 16);
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    auto addr_in = GetValue<SockAddrIn>(addr);

    return Translate(descriptor->socket->Bind(Translate(addr_in)));
}

Errno BSD_USA::ConnectImpl(s32 fd, std::span<const u8> addr) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    ASSERT(addr.size() >= 16);
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    auto addr_in = GetValue<SockAddrIn>(addr);

    const auto destination = Network::NsoLan::RouteDestination(
        Translate(addr_in), !descriptor->is_connection_based);
    if (!destination) {
        LOG_WARNING(Service, "NSO LAN fixture denied an unconfigured destination");
        return Errno::CONNREFUSED;
    }
    const Errno result = Translate(descriptor->socket->Connect(*destination));

    if (result == Errno::ISCONN) {
        LOG_DEBUG(Service, "returned ISCONN - socket already connected");
        return Errno::SUCCESS;
    }

    return result;
}

Errno BSD_USA::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = descriptor->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD_USA::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = descriptor->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD_USA::ListenImpl(s32 fd, s32 backlog) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }
    if (Network::NsoLan::GetAutomaticTransport()) {
        return Errno::INVAL; // Automatic mode exposes no guest TCP listeners to the host LAN.
    }
    return Translate(descriptor->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD_USA::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }

    switch (cmd) {
    case FcntlCmd::GETFL:
        ASSERT(arg == 0);
        return {descriptor->flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        const Errno bsd_errno = Translate(descriptor->socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        // A concurrent Close may have released fd for reuse; never publish to the replacement.
        if (!file_descriptors.SetFlags(fd, descriptor->serial, arg)) {
            return {-1, Errno::BADF};
        }
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

Errno BSD_USA::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        UNIMPLEMENTED_MSG("Unknown getsockopt level");
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = descriptor->socket.get();

    switch (optname) {
    case OptName::ERROR_: {
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            Errno translated_pending_err = Translate(pending_err);
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(Errno), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(Errno));
            PutValue(optval, translated_pending_err);
        }
        return Translate(getsockopt_err);
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented optname={}", optname);
        return Errno::SUCCESS;
    }
}

Errno BSD_USA::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service, "(STUBBED) setsockopt with level={}, optname={}", level, optname);
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = descriptor->socket.get();

    if (optname == OptName::LINGER) {
        ASSERT(optval.size() == sizeof(Linger));
        auto linger = GetValue<Linger>(optval);
        ASSERT(linger.onoff == 0 || linger.onoff == 1);

        return Translate(socket->SetLinger(linger.onoff != 0, linger.linger));
    }

    ASSERT(optval.size() == sizeof(u32));
    auto value = GetValue<u32>(optval);

    switch (optname) {
    case OptName::REUSEADDR:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetReuseAddr(value != 0));
    case OptName::KEEPALIVE:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetKeepAlive(value != 0));
    case OptName::BROADCAST:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetBroadcast(value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(value));
    case OptName::SNDTIMEO:
        return Translate(socket->SetSndTimeo(value));
    case OptName::RCVTIMEO:
        return Translate(socket->SetRcvTimeo(value));
    case OptName::NOSIGPIPE:
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", value);
        return Errno::SUCCESS;
    default:
        UNIMPLEMENTED_MSG("Unimplemented optname={}", optname);
        return Errno::SUCCESS;
    }
}

Errno BSD_USA::ShutdownImpl(s32 fd, s32 how) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }
    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(descriptor->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD_USA::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    // The snapshot owns the socket through the blocking receive: a concurrent Close may wake it
    // and drop the table's reference before this returns.
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor->flags & FLAG_O_NONBLOCK) == 0) {
            descriptor->socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor->socket->Recv(flags, message));

    // Restore original state, unless the descriptor was closed (and possibly reused) meanwhile
    const auto current_flags = file_descriptors.GetFlags(fd, descriptor->serial);
    if (current_flags && (*current_flags & FLAG_O_NONBLOCK) == 0) {
        descriptor->socket->SetNonBlock(false);
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr) {
    // See RecvImpl: the snapshot keeps the socket alive through a Close during the receive.
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor->is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else {
        p_addr_in = &addr_in;
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor->flags & FLAG_O_NONBLOCK) == 0) {
            descriptor->socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] =
        Translate(descriptor->socket->RecvFrom(flags, message, p_addr_in));

    // Restore original state, unless the descriptor was closed (and possibly reused) meanwhile
    const auto current_flags = file_descriptors.GetFlags(fd, descriptor->serial);
    if (current_flags && (*current_flags & FLAG_O_NONBLOCK) == 0) {
        descriptor->socket->SetNonBlock(false);
    }

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            ASSERT(addr.size() >= 16);
            const SockAddrIn result = Translate(addr_in);
            PutValue(addr, result);
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }
    return Translate(descriptor->socket->Send(message, flags));
}

std::pair<s32, Errno> BSD_USA::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return {-1, Errno::BADF};
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }

    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        ASSERT(addr.size() >= 16);
        auto guest_addr_in = GetValue<SockAddrIn>(addr);
        addr_in = Translate(guest_addr_in);
        const auto destination = Network::NsoLan::RouteDestination(
            addr_in, !descriptor->is_connection_based);
        if (!destination) {
            return {-1, Errno::CONNREFUSED};
        }
        addr_in = *destination;
        p_addr_in = &addr_in;
    }

    return Translate(descriptor->socket->SendTo(flags, message, p_addr_in));
}

Errno BSD_USA::CloseImpl(s32 fd) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    // Close outside the table lock; it may wake a receiver that still holds its own snapshot.
    const Errno bsd_errno = Translate(descriptor->socket->Close());
    if (bsd_errno != Errno::SUCCESS) {
        return bsd_errno;
    }

    // Only remove the publication that was closed; a concurrent Close may already have freed fd
    // and another thread may have been given the same number.
    if (!file_descriptors.Erase(fd, descriptor->serial)) {
        return Errno::BADF;
    }

    LOG_INFO(Service, "Close socket fd={}", fd);
    return bsd_errno;
}

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(s32 fd) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return Errno::BADF;
    }

    const s32 new_fd = file_descriptors.Insert(*descriptor);
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Errno::MFILE;
    }
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD_USA::GetSocket(s32 fd) {
    const auto descriptor = GetDescriptor(fd);
    if (!descriptor) {
        return std::nullopt;
    }
    if (!descriptor->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return std::nullopt;
    }
    return descriptor->socket;
}

std::optional<DescriptorTable::Entry> BSD_USA::GetDescriptor(s32 fd) const {
    if (!DescriptorTable::InRange(fd)) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return std::nullopt;
    }
    auto descriptor = file_descriptors.Get(fd);
    if (!descriptor) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
    }
    return descriptor;
}

void BSD_USA::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    // Deliver without the table lock; the snapshot keeps each socket alive through its handler.
    for (const auto& socket : file_descriptors.Sockets()) {
        socket->HandleProxyPacket(packet);
    }
}

BSD_USA::BSD_USA(Core::System& system_, const char* name, bool is_user_)
    : ServiceFramework{system_, name}
    , is_user{is_user_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD_USA::RegisterClient, "RegisterClient"},
        {1, &BSD_USA::StartMonitoring, "StartMonitoring"},
        {2, &BSD_USA::Socket, "Socket"},
        {3, &BSD_USA::SocketExempt, "SocketExempt"},
        {4, nullptr, "Open"},
        {5, &BSD_USA::Select, "Select"},
        {6, &BSD_USA::Poll, "Poll"},
        {7, nullptr, "Sysctl"},
        {8, &BSD_USA::Recv, "Recv"},
        {9, &BSD_USA::RecvFrom, "RecvFrom"},
        {10, &BSD_USA::Send, "Send"},
        {11, &BSD_USA::SendTo, "SendTo"},
        {12, &BSD_USA::Accept, "Accept"},
        {13, &BSD_USA::Bind, "Bind"},
        {14, &BSD_USA::Connect, "Connect"},
        {15, &BSD_USA::GetPeerName, "GetPeerName"},
        {16, &BSD_USA::GetSockName, "GetSockName"},
        {17, &BSD_USA::GetSockOpt, "GetSockOpt"},
        {18, &BSD_USA::Listen, "Listen"},
        {19, nullptr, "Ioctl"},
        {20, &BSD_USA::Fcntl, "Fcntl"},
        {21, &BSD_USA::SetSockOpt, "SetSockOpt"},
        {22, &BSD_USA::Shutdown, "Shutdown"},
        {23, nullptr, "ShutdownAllSockets"},
        {24, &BSD_USA::Write, "Write"},
        {25, &BSD_USA::Read, "Read"},
        {26, &BSD_USA::Close, "Close"},
        {27, &BSD_USA::DuplicateSocket, "DuplicateSocket"},
        {28, nullptr, "GetResourceStatistics"},
        {29, nullptr, "RecvMMsg"}, //3.0.0+
        {30, nullptr, "SendMMsg"}, //3.0.0+
        {31, &BSD_USA::EventFd, "EventFd"}, //7.0.0+
        {32, nullptr, "RegisterResourceStatisticsName"}, //7.0.0+
        {33, nullptr, "RegisterClientShared"}, //10.0.0+
        {34, nullptr, "GetSocketStatistics"}, //15.0.0+
        {35, nullptr, "NifIoctl"}, //17.0.0+
        {36, nullptr, "Unknown36"}, //18.0.0+
        {37, nullptr, "Unknown37"}, //18.0.0+
        {38, nullptr, "Unknown38"}, //18.0.0+
        {39, nullptr, "Unknown39"}, //20.0.0+
        {40, nullptr, "Unknown40"}, //20.0.0+
        {41, nullptr, "Unknown41"}, //21.0.0+
        {42, nullptr, "Unknown42"}, //21.0.0+
        {43, nullptr, "Unknown43"}, //21.0.0+
        {200, nullptr, "SetThreadCoreMask"}, //15.0.0+
        {201, nullptr, "GetThreadCoreMask"}, //15.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);

    if (auto room_member = Network::GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD_USA::~BSD_USA() {
    if (auto room_member = Network::GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }
}

std::unique_lock<std::mutex> BSD_USA::LockService() noexcept {
    return {};
}

BSDCFG::BSDCFG(Core::System& system_, const char *name)
    : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetIfUp"},
        {1, nullptr, "SetIfUpWithEvent"},
        {2, nullptr, "CancelIf"},
        {3, nullptr, "SetIfDown"},
        {4, nullptr, "GetIfState"},
        {5, nullptr, "DhcpRenew"},
        {6, nullptr, "AddStaticArpEntry"},
        {7, nullptr, "RemoveArpEntry"},
        {8, nullptr, "LookupArpEntry"},
        {9, nullptr, "LookupArpEntry2"},
        {10, nullptr, "ClearArpEntries"},
        {11, nullptr, "ClearArpEntries2"},
        {12, nullptr, "PrintArpEntries"},
        {13, nullptr, "Unknown13"},
        {14, nullptr, "Unknown14"},
        {15, nullptr, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

BSD_NU::BSD_NU(Core::System& system_)
    : ServiceFramework{system_, "bsd:nu"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CreateUserService"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

BSD_NU::~BSD_NU() = default;

} // namespace Service::Sockets
