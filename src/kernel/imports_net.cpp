// Network: the console's NetDll_* imports over host Winsock.
//
// Off by default (the legacy behaviour: no link, every call fails cleanly). FM2_ONLINE=1 turns the
// link on: the title sees an Ethernet link with a static IPv4 address, XNADDR/INADDR map to plain
// IPv4 addresses, and sockets are host sockets. Guest sockaddr_in and in_addr values are already
// big-endian, the order the wire uses, so addresses and ports are copied byte for byte.
//
// Secure-network calls (XNetCreateKey/RegisterKey/Connect) succeed without a tunnel: peers talk
// plain UDP/TCP. The title server's address comes from XNetServerToInAddr (FM2_SERVER_ADDR, default
// 127.0.0.1).
//
// Not yet covered: overlapped completion for WSASendTo/WSARecvFrom (done synchronously),
// WSASend/WSARecv, QoS probing (see XNetQosLookup).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "kernel/imports.h"
#include "kernel/objects.h"
#include "live_switch.h"
#include "log.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
LiveSwitch g_online("FM2_ONLINE");

constexpr uint32_t kInvalidSocket = 0xFFFFFFFF;
constexpr uint32_t kSocketError = 0xFFFFFFFF;

std::mutex g_mutex;
std::vector<SOCKET> g_sockets; // guest handle = index + 1 (0 is never a socket)
thread_local int t_lastError = 0;
bool g_wsaUp = false;

void EnsureWsa()
{
    std::lock_guard lock(g_mutex);
    if (g_wsaUp)
        return;
    WSADATA d;
    g_wsaUp = WSAStartup(MAKEWORD(2, 2), &d) == 0;
}

uint32_t NewHandle(SOCKET s)
{
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < g_sockets.size(); i++)
        if (g_sockets[i] == INVALID_SOCKET)
        {
            g_sockets[i] = s;
            return uint32_t(i + 1);
        }
    g_sockets.push_back(s);
    return uint32_t(g_sockets.size());
}

SOCKET Host(uint32_t handle)
{
    std::lock_guard lock(g_mutex);
    return handle && handle <= g_sockets.size() ? g_sockets[handle - 1] : INVALID_SOCKET;
}

uint32_t Fail(PPCContext& ctx, int err = 0)
{
    t_lastError = err ? err : WSAGetLastError();
    Return(ctx, kSocketError);
    return kSocketError;
}

// Guest sockaddr_in: { u16 family, u16 port, u32 addr, u8 zero[8] }, all big-endian.
sockaddr_in ToHost(uint32_t guest)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    memcpy(&a.sin_port, GuestPtr<uint8_t>(guest + 2), 2);
    memcpy(&a.sin_addr, GuestPtr<uint8_t>(guest + 4), 4);
    return a;
}

void FromHost(uint32_t guest, const sockaddr_in& a)
{
    uint8_t* p = GuestPtr<uint8_t>(guest);
    memset(p, 0, 16);
    p[1] = 2; // AF_INET, big-endian
    memcpy(p + 2, &a.sin_port, 2);
    memcpy(p + 4, &a.sin_addr, 4);
}

// The address this machine presents as the console's: FM2_NET_ADDR, else the first IPv4 address
// the host resolves for itself, else loopback. Network byte order.
uint32_t LocalAddr()
{
    static const uint32_t addr = [] {
        EnsureWsa();
        if (const char* e = getenv("FM2_NET_ADDR"))
        {
            in_addr a{};
            if (inet_pton(AF_INET, e, &a) == 1)
                return uint32_t(a.s_addr);
        }
        char name[256];
        if (gethostname(name, sizeof name) == 0)
        {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(name, nullptr, &hints, &res) == 0 && res)
            {
                const uint32_t a = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(res);
                return a;
            }
        }
        return uint32_t(htonl(INADDR_LOOPBACK));
    }();
    return addr;
}

uint32_t ServerAddr()
{
    static const uint32_t addr = [] {
        if (const char* e = getenv("FM2_SERVER_ADDR"))
        {
            in_addr a{};
            if (inet_pton(AF_INET, e, &a) == 1)
                return uint32_t(a.s_addr);
        }
        return uint32_t(htonl(INADDR_LOOPBACK));
    }();
    return addr;
}

// The title talks HTTP on port 80 of the title server; ours listens on FM2_SERVER_PORT (8080).
void RedirectTitleServer(sockaddr_in& a)
{
    if (a.sin_addr.s_addr != ServerAddr() || ntohs(a.sin_port) != 80)
        return;
    static const unsigned short port = [] {
        const char* e = getenv("FM2_SERVER_PORT");
        const int p = e ? atoi(e) : 8080;
        return (unsigned short)(p > 0 && p < 65536 ? p : 8080);
    }();
    a.sin_port = htons(port);
}

void StoreRaw32(uint32_t guest, uint32_t networkOrder)
{
    memcpy(GuestPtr<uint8_t>(guest), &networkOrder, 4);
}

uint32_t LoadRaw32(uint32_t guest)
{
    uint32_t v;
    memcpy(&v, GuestPtr<uint8_t>(guest), 4);
    return v;
}

// Overlapped datagram receives ---------------------------------------------------------------
//
// The title's network worker calls WSARecvFrom with an overlapped and expects WSA_IO_PENDING (997) when no datagram
// is waiting, then waits on the overlapped's event and collects the result with WSAGetOverlappedResult. A blocking
// host recvfrom there parks the worker (and everything that waits on it, the main thread included) for good. A
// call with an overlapped now answers at once when a datagram is ready, else it is queued and a host thread
// completes it when the socket turns readable: the guest buffer, from address, byte counts and overlapped are
// written, then the overlapped's event is signalled. The guest overlapped is { u32 Internal; u32 InternalHigh;
// u32 Offset; u32 OffsetHigh; u32 hEvent } (hEvent at +0x10); an XOVERLAPPED-style event at +0x0C is honoured too.
constexpr int kWsaIoPending = 997;
constexpr int kWsaOperationAborted = 995;
constexpr uint32_t kStatusPending = 0x103;

struct PendingRecv
{
    uint32_t handle;
    uint32_t buffer, length;
    uint32_t received; // DWORD* for the byte count
    uint32_t from, fromLen;
    uint32_t overlapped;
};
struct OverlappedState
{
    bool done = false;
    uint32_t bytes = 0;
    int error = 0;
};

std::mutex g_ioMutex;
std::condition_variable g_ioCv;
std::vector<PendingRecv> g_pending;
std::unordered_map<uint32_t, OverlappedState> g_overlapped; // by guest overlapped address
bool g_ioThreadUp = false;

void SignalOverlappedEvent(uint32_t overlapped)
{
    for (const uint32_t offset : { 0x10u, 0x0Cu })
    {
        const uint32_t event = LoadBE32(overlapped + offset);
        if (!event)
            continue;
        auto object = GetHandle(event);
        if (!object || object->kind != KernelObject::Kind::Event)
            continue;
        {
            std::lock_guard lock(g_objectMutex);
            static_cast<EventObject&>(*object).signaled = true;
            SyncGuestSignalState(*object);
        }
        SignalObjectsChanged();
        return;
    }
}

// Records the outcome in the guest overlapped and the host table, then signals the event.
void CompleteGuestOverlapped(uint32_t overlapped, uint32_t bytes, int error)
{
    if (!overlapped)
        return;
    StoreBE32(overlapped + 0x00, error ? uint32_t(0xC0000000 | error) : 0u);
    StoreBE32(overlapped + 0x04, bytes);
    {
        std::lock_guard lock(g_ioMutex);
        OverlappedState& st = g_overlapped[overlapped];
        st.done = true;
        st.bytes = bytes;
        st.error = error;
    }
    SignalOverlappedEvent(overlapped);
}

void MarkOverlappedPending(uint32_t overlapped)
{
    StoreBE32(overlapped + 0x00, kStatusPending);
    StoreBE32(overlapped + 0x04, 0);
    std::lock_guard lock(g_ioMutex);
    g_overlapped[overlapped] = OverlappedState{};
}

// Reads one datagram that is known to be ready into the guest request. Returns the byte count, or -1 with the host error.
int ReceiveInto(SOCKET s, const PendingRecv& r)
{
    sockaddr_in from{};
    int fromLen = sizeof from;
    const int n = recvfrom(s, GuestPtr<char>(r.buffer), int(r.length), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n < 0)
        return -1;
    if (r.received)
        StoreBE32(r.received, uint32_t(n));
    if (r.from)
    {
        FromHost(r.from, from);
        if (r.fromLen)
            StoreBE32(r.fromLen, 16);
    }
    return n;
}

void IoThreadMain()
{
    for (;;)
    {
        std::vector<PendingRecv> snapshot;
        {
            std::unique_lock lock(g_ioMutex);
            if (g_pending.empty())
                g_ioCv.wait_for(lock, std::chrono::milliseconds(100));
            snapshot = g_pending;
        }
        if (snapshot.empty())
            continue;
        fd_set readable;
        FD_ZERO(&readable);
        int count = 0;
        for (const PendingRecv& r : snapshot)
            if (const SOCKET s = Host(r.handle); s != INVALID_SOCKET && count < FD_SETSIZE)
            {
                FD_SET(s, &readable);
                count++;
            }
        if (!count)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        timeval timeout{ 0, 20000 };
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
            continue;
        for (const PendingRecv& r : snapshot)
        {
            const SOCKET s = Host(r.handle);
            if (s == INVALID_SOCKET || !FD_ISSET(s, &readable))
                continue;
            {
                // Still queued? (closesocket may have cancelled it.)
                std::lock_guard lock(g_ioMutex);
                bool found = false;
                for (size_t i = 0; i < g_pending.size(); i++)
                    if (g_pending[i].overlapped == r.overlapped && g_pending[i].handle == r.handle)
                    {
                        g_pending.erase(g_pending.begin() + i);
                        found = true;
                        break;
                    }
                if (!found)
                    continue;
            }
            const int n = ReceiveInto(s, r);
            if (g_traceDnet)
                Log("dnet: ws recv completed: socket %u overlapped %08X bytes %d", r.handle, r.overlapped, n);
            CompleteGuestOverlapped(r.overlapped, n < 0 ? 0u : uint32_t(n), n < 0 ? WSAGetLastError() : 0);
        }
    }
}

void QueueReceive(const PendingRecv& r)
{
    MarkOverlappedPending(r.overlapped);
    {
        std::lock_guard lock(g_ioMutex);
        g_pending.push_back(r);
        if (!g_ioThreadUp)
        {
            g_ioThreadUp = true;
            std::thread(IoThreadMain).detach();
        }
    }
    g_ioCv.notify_all();
}

// Completes every receive still queued on a socket that is being closed.
void CancelReceives(uint32_t handle)
{
    std::vector<PendingRecv> cancelled;
    {
        std::lock_guard lock(g_ioMutex);
        for (size_t i = 0; i < g_pending.size();)
            if (g_pending[i].handle == handle)
            {
                cancelled.push_back(g_pending[i]);
                g_pending.erase(g_pending.begin() + i);
            }
            else
                i++;
    }
    for (const PendingRecv& r : cancelled)
    {
        if (g_traceDnet)
            Log("dnet: ws recv cancelled: socket %u overlapped %08X", r.handle, r.overlapped);
        CompleteGuestOverlapped(r.overlapped, 0, kWsaOperationAborted);
    }
}
} // namespace

bool NetOnline()
{
    return g_online;
}

uint32_t NetServerAddr()
{
    return ServerAddr();
}

// Startup and status -----------------------------------------------------------------------

GUEST_EXPORT(NetDll_XNetStartup)
{
    if (g_online)
        EnsureWsa();
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_XNetCleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAStartup)
{
    // (caller, version, WSADATA*): the title reads wVersion back and fails its network stack init (and destroys the
    // network manager the later session code copies) unless it is the version it asked for. The console's WSADATA:
    // { u16 wVersion, wHighVersion; char szDescription[257]; char szSystemStatus[129]; u16 iMaxSockets, iMaxUdpDg;
    // char* lpVendorInfo } (400 bytes with its padding), big-endian.
    if (g_online)
    {
        EnsureWsa();
        if (const uint32_t data = ctx.r5.u32)
        {
            constexpr uint16_t kHighVersion = 0x0202;
            const uint16_t requested = uint16_t(ctx.r4.u32);
            const uint16_t version = requested <= kHighVersion ? requested : kHighVersion;
            uint8_t* d = GuestPtr<uint8_t>(data);
            memset(d, 0, 400);
            d[0] = uint8_t(version >> 8);
            d[1] = uint8_t(version);
            d[2] = uint8_t(kHighVersion >> 8);
            d[3] = uint8_t(kHighVersion);
            memcpy(d + 4, "WinSock 2.0", 12); // szDescription
        }
    }
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_WSACleanup) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_WSAGetLastError) { Return(ctx, g_online ? uint32_t(t_lastError) : 10050u); } // WSAENETDOWN offline

GUEST_EXPORT(NetDll_XNetGetEthernetLinkStatus)
{
    // XNET_ETHERNET_LINK_ACTIVE | 100MBPS | FULL_DUPLEX
    Return(ctx, g_online ? 0x0Bu : 0u);
}

GUEST_EXPORT(NetDll_XNetGetTitleXnAddr)
{
    // (caller, XNADDR*): { in_addr ina, in_addr inaOnline, u16 wPortOnline, u8 abEnet[6], u8 abOnline[20] }
    if (!g_online)
    {
        Return(ctx, 1); // XNET_GET_XNADDR_NONE
        return;
    }
    uint8_t* x = GuestPtr<uint8_t>(ctx.r4.u32);
    memset(x, 0, 36);
    StoreRaw32(ctx.r4.u32, LocalAddr());
    const uint32_t a = LocalAddr(); // a stable fake MAC from the address
    x[10] = 0x00; x[11] = 0x22; x[12] = 0x48;
    memcpy(x + 13, reinterpret_cast<const uint8_t*>(&a) + 1, 3);
    Return(ctx, 0x02 | 0x04); // XNET_GET_XNADDR_ETHERNET | STATIC
}

GUEST_EXPORT(NetDll_XNetXnAddrToMachineId)
{
    // (caller, XNADDR*, uint64* machineId): a stable id built from the address.
    if (!g_online)
    {
        Return(ctx, 10050); // WSAENETDOWN
        return;
    }
    const uint32_t address = LoadBE32(ctx.r4.u32);
    StoreBE64(ctx.r5.u32, 0x00FA000000000000ull | address);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetGetConnectStatus)
{
    Return(ctx, g_online ? 2u : 3u); // connected : lost
}

// Addresses and keys ----------------------------------------------------------------------

GUEST_EXPORT(NetDll_XNetServerToInAddr)
{
    // (caller, in_addr ina, serviceId, in_addr* out): the title server is the configured host.
    if (!g_online)
    {
        Return(ctx, 10050);
        return;
    }
    StoreRaw32(ctx.r6.u32, ServerAddr());
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetXnAddrToInAddr)
{
    // (caller, XNADDR*, XNKID*, in_addr*): the secure address is the plain address.
    if (!g_online)
    {
        Return(ctx, 10050);
        return;
    }
    StoreRaw32(ctx.r6.u32, LoadRaw32(ctx.r4.u32));
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetInAddrToXnAddr)
{
    // (caller, in_addr, XNADDR*, XNKID*)
    if (!g_online)
    {
        Return(ctx, 10050);
        return;
    }
    uint8_t* x = GuestPtr<uint8_t>(ctx.r5.u32);
    memset(x, 0, 36);
    StoreRaw32(ctx.r5.u32, ctx.r4.u32 ? uint32_t(__builtin_bswap32(ctx.r4.u32)) : 0);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_XNetInAddrToString)
{
    // (caller, in_addr, char* buf, len): in_addr is passed by value, in network order as a guest u32.
    const uint32_t netOrder = __builtin_bswap32(ctx.r4.u32);
    char text[16];
    in_addr a{};
    a.s_addr = netOrder;
    inet_ntop(AF_INET, &a, text, sizeof text);
    const uint32_t cap = ctx.r6.u32;
    if (cap)
        strncpy(GuestPtr<char>(ctx.r5.u32), text, cap - 1), GuestPtr<char>(ctx.r5.u32)[cap - 1] = 0;
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_inet_addr)
{
    in_addr a{};
    if (inet_pton(AF_INET, GuestString(ctx.r4.u32).c_str(), &a) == 1)
        Return(ctx, __builtin_bswap32(uint32_t(a.s_addr)));
    else
        Return(ctx, 0xFFFFFFFFu);
}

GUEST_EXPORT(NetDll_XNetCreateKey)
{
    // (caller, XNKID* id, XNKEY* key): a random 8-byte id (multicast bit clear, system-link bit set) and 16-byte key.
    static std::mt19937_64 rng{ std::random_device{}() };
    uint8_t* id = GuestPtr<uint8_t>(ctx.r4.u32);
    uint8_t* key = GuestPtr<uint8_t>(ctx.r5.u32);
    for (int i = 0; i < 8; i++) id[i] = uint8_t(rng());
    id[0] = (id[0] & 0x3F) | 0x80;
    for (int i = 0; i < 16; i++) key[i] = uint8_t(rng());
    Return(ctx, 0);
}
GUEST_EXPORT(NetDll_XNetRegisterKey) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetUnregisterKey) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetUnregisterInAddr) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetConnect) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetQosListen) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetQosRelease) { Return(ctx, 0); }
GUEST_EXPORT(NetDll_XNetQosLookup)
{
    // No QoS probing yet: the lookup fails and the title falls back to its own pings.
    Return(ctx, 10022); // WSAEINVAL
}

// Sockets ---------------------------------------------------------------------------------

GUEST_EXPORT(NetDll_socket)
{
    // (caller, af, type, protocol); the console's VDP protocol (254) is plain UDP here.
    if (!g_online)
    {
        Return(ctx, kInvalidSocket);
        return;
    }
    EnsureWsa();
    const int type = int(ctx.r5.u32);
    int proto = int(ctx.r6.u32);
    if (proto == 254)
        proto = IPPROTO_UDP;
    SOCKET s = socket(AF_INET, type, proto);
    if (s == INVALID_SOCKET)
    {
        Fail(ctx);
        return;
    }
    Return(ctx, NewHandle(s));
}

GUEST_EXPORT(NetDll_closesocket)
{
    if (!g_online)
    {
        Return(ctx, 0);
        return;
    }
    const uint32_t h = ctx.r4.u32;
    SOCKET s = Host(h);
    if (s == INVALID_SOCKET)
    {
        Fail(ctx, WSAENOTSOCK);
        return;
    }
    CancelReceives(h);
    {
        std::lock_guard lock(g_mutex);
        g_sockets[h - 1] = INVALID_SOCKET;
    }
    closesocket(s);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_shutdown)
{
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    Return(ctx, shutdown(s, int(ctx.r5.u32)) == 0 ? 0u : Fail(ctx));
}

GUEST_EXPORT(NetDll_bind)
{
    // (caller, s, sockaddr*, len)
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    sockaddr_in a = ToHost(ctx.r5.u32);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { Fail(ctx); return; }
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_connect)
{
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    sockaddr_in a = ToHost(ctx.r5.u32);
    RedirectTitleServer(a);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { Fail(ctx); return; }
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_getsockname)
{
    // (caller, s, sockaddr*, int* len)
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    sockaddr_in a{};
    int len = sizeof a;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) { Fail(ctx); return; }
    FromHost(ctx.r5.u32, a);
    StoreIfNonNull(ctx.r6.u32, 16);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_send)
{
    // (caller, s, buf, len, flags)
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    const int n = send(s, GuestPtr<const char>(ctx.r5.u32), int(ctx.r6.u32), int(ctx.r7.u32));
    if (n < 0) { Fail(ctx); return; }
    Return(ctx, uint32_t(n));
}

GUEST_EXPORT(NetDll_recv)
{
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    const int n = recv(s, GuestPtr<char>(ctx.r5.u32), int(ctx.r6.u32), int(ctx.r7.u32));
    if (n < 0) { Fail(ctx); return; }
    Return(ctx, uint32_t(n));
}

GUEST_EXPORT(NetDll_ioctlsocket)
{
    // (caller, s, cmd, u_long* arg): FIONBIO and FIONREAD have the same values on the console.
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    u_long v = LoadBE32(ctx.r6.u32);
    if (ioctlsocket(s, long(ctx.r5.u32), &v) != 0) { Fail(ctx); return; }
    StoreBE32(ctx.r6.u32, uint32_t(v));
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_setsockopt)
{
    // (caller, s, level, name, value*, len): console-only options (XSO_*, > 0xFFFF) are accepted and ignored.
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    const int level = int(ctx.r5.u32), name = int(ctx.r6.u32);
    const uint32_t len = ctx.r8.u32;
    if (name > 0xFFFF || (len != 4 && len != 1))
    {
        Return(ctx, 0);
        return;
    }
    int v = len == 4 ? int(LoadBE32(ctx.r7.u32)) : int(*GuestPtr<uint8_t>(ctx.r7.u32));
    setsockopt(s, level, name, reinterpret_cast<const char*>(&v), sizeof v); // best effort
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_getsockopt)
{
    // (caller, s, level, name, value*, int* len)
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    int v = 0, len = sizeof v;
    if (int(ctx.r6.u32) <= 0xFFFF)
        getsockopt(s, int(ctx.r5.u32), int(ctx.r6.u32), reinterpret_cast<char*>(&v), &len);
    StoreBE32(ctx.r7.u32, uint32_t(v));
    StoreIfNonNull(ctx.r8.u32, 4);
    Return(ctx, 0);
}

// fd_set on the console: { u32 count; u32 handles[64] }, big-endian.
GUEST_EXPORT(NetDll___WSAFDIsSet)
{
    // (socket, fd_set*): __WSAFDIsSet has no caller argument.
    const uint32_t set = ctx.r4.u32, n = set ? LoadBE32(set) : 0;
    for (uint32_t i = 0; i < n && i < 64; i++)
        if (LoadBE32(set + 4 + i * 4) == ctx.r3.u32)
        {
            Return(ctx, 1);
            return;
        }
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_select)
{
    // (caller, nfds, readfds, writefds, exceptfds, timeval*)
    fd_set host[3];
    uint32_t guest[3] = { ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 };
    fd_set* hp[3] = { nullptr, nullptr, nullptr };
    for (int k = 0; k < 3; k++)
    {
        if (!guest[k])
            continue;
        FD_ZERO(&host[k]);
        const uint32_t n = LoadBE32(guest[k]);
        for (uint32_t i = 0; i < n && i < 64; i++)
            if (SOCKET s = Host(LoadBE32(guest[k] + 4 + i * 4)); s != INVALID_SOCKET)
                FD_SET(s, &host[k]);
        hp[k] = &host[k];
    }
    timeval tv{}, *tvp = nullptr;
    if (const uint32_t t = ctx.r8.u32)
    {
        tv.tv_sec = long(LoadBE32(t));
        tv.tv_usec = long(LoadBE32(t + 4));
        tvp = &tv;
    }
    const int r = select(0, hp[0], hp[1], hp[2], tvp);
    if (r < 0) { Fail(ctx); return; }
    for (int k = 0; k < 3; k++)
    {
        if (!guest[k])
            continue;
        const uint32_t n = LoadBE32(guest[k]);
        uint32_t out = 0;
        for (uint32_t i = 0; i < n && i < 64; i++)
        {
            const uint32_t h = LoadBE32(guest[k] + 4 + i * 4);
            if (SOCKET s = Host(h); s != INVALID_SOCKET && FD_ISSET(s, &host[k]))
                StoreBE32(guest[k] + 4 + out++ * 4, h);
        }
        StoreBE32(guest[k], out);
    }
    Return(ctx, uint32_t(r));
}

// Datagrams: WSABUF is { u32 len; u32 buf }. Sends complete at once (an overlapped argument gets its result and event
// set on the spot, its completion routine is not run). Receives with an overlapped may stay queued (see above); without
// one they block the calling thread like the plain call.
GUEST_EXPORT(NetDll_WSASendTo)
{
    // (caller, s, WSABUF*, count, sent*, flags, to*, tolen, overlapped, completion)
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    sockaddr_in to = ToHost(ctx.r9.u32);
    RedirectTitleServer(to);
    uint32_t total = 0;
    for (uint32_t i = 0; i < ctx.r6.u32; i++)
    {
        const uint32_t b = ctx.r5.u32 + i * 8;
        const int n = sendto(s, GuestPtr<const char>(LoadBE32(b + 4)), int(LoadBE32(b)), int(ctx.r8.u32),
                             reinterpret_cast<sockaddr*>(&to), sizeof to);
        if (n < 0) { Fail(ctx); return; }
        total += uint32_t(n);
    }
    StoreIfNonNull(ctx.r7.u32, total);
    if (const uint32_t overlapped = StackArg(ctx, 8))
        CompleteGuestOverlapped(overlapped, total, 0);
    Return(ctx, 0);
}

GUEST_EXPORT(NetDll_WSARecvFrom)
{
    // (caller, s, WSABUF*, count, received*, flags*, from*, fromlen*, overlapped, completion): one datagram
    SOCKET s = Host(ctx.r4.u32);
    if (s == INVALID_SOCKET) { Fail(ctx, WSAENOTSOCK); return; }
    const uint32_t b = ctx.r5.u32;
    const uint32_t overlapped = StackArg(ctx, 8);
    PendingRecv r{ ctx.r4.u32, LoadBE32(b + 4), LoadBE32(b), ctx.r7.u32, ctx.r9.u32, ctx.r10.u32, overlapped };
    if (!overlapped)
    {
        const int n = ReceiveInto(s, r);
        if (n < 0) { Fail(ctx); return; }
        Return(ctx, 0);
        return;
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval none{ 0, 0 };
    if (select(0, &readable, nullptr, nullptr, &none) > 0)
    {
        const int n = ReceiveInto(s, r);
        if (n < 0) { Fail(ctx); return; }
        CompleteGuestOverlapped(overlapped, uint32_t(n), 0);
        Return(ctx, 0);
        return;
    }
    if (g_traceDnet)
        Log("dnet: ws recv queued: socket %u overlapped %08X buffer %08X len %u (lr %08llX)", r.handle, overlapped, r.buffer, r.length,
            (unsigned long long)ctx.lr);
    QueueReceive(r);
    Fail(ctx, kWsaIoPending);
}

GUEST_EXPORT(NetDll_WSAGetOverlappedResult)
{
    // (caller, s, overlapped, bytes*, wait, flags*) -> BOOL
    const uint32_t overlapped = ctx.r5.u32;
    OverlappedState st;
    bool known = false;
    for (;;)
    {
        {
            std::lock_guard lock(g_ioMutex);
            auto it = g_overlapped.find(overlapped);
            known = it != g_overlapped.end();
            if (known)
                st = it->second;
        }
        if (!known || st.done || !ctx.r7.u32)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // wait = TRUE: until the receive completes
    }
    if (!known)
    {
        // Not an operation we queued: its result was written by the call that completed it.
        StoreIfNonNull(ctx.r6.u32, LoadBE32(overlapped + 4));
        StoreIfNonNull(ctx.r8.u32, 0);
        Return(ctx, 1);
        return;
    }
    if (!st.done)
    {
        t_lastError = kWsaIoPending; // WSA_IO_INCOMPLETE
        Return(ctx, 0);
        return;
    }
    StoreIfNonNull(ctx.r6.u32, st.bytes);
    StoreIfNonNull(ctx.r8.u32, 0);
    if (st.error)
    {
        t_lastError = st.error;
        Return(ctx, 0);
        return;
    }
    Return(ctx, 1);
}
