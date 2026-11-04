// FM4_TRACE_DNET=1: trace the title's network-manager singleton (the guest word at 0x82E9D3F0) and the objects that
// copy it. sub_82CE31C0 is its get-or-create (sub_82CE35F8 and sub_82CEAE58 call it); its refcount is the guest word at
// +4 (incremented by the get-or-create, decremented by sub_82CE35A0, which calls the destroy sub_82CE34A0 at 0; the
// destroy zeroes the global; sub_82CE36B0 destroys it unconditionally and is part of the stack shutdown sub_82CBD550).
// The "DNET" object built by sub_82CBE348 copies the global into its +164 (sub_82CDF870) without creating it; if the
// global is still null then, the first virtual call on the object (slot 3, sub_82CD0808) ends in an indirect call to 0
// at lr 82CD08C0. Each line has the guest thread register r13, the host thread id, the singleton and its refcount, and
// where it matters a guest call chain (return addresses walked up the stack's back chain).
// Off by default; read-only (the hooks call the original and only log).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

bool g_traceDnet = getenv("FM4_TRACE_DNET") != nullptr;

namespace
{
constexpr uint32_t kSingleton = 0x82E9D3F0;
thread_local int t_createDepth; // inside sub_82CE31C0 on this thread

bool InImage(uint32_t a) { return a >= 0x82000000 && a < 0x8C000000; }
bool OnStack(uint32_t a) { return a >= 0x40000000 && a < 0x80000000 && (a & 3) == 0; }

// Return addresses of the callers above the hooked function: the frame pointer r1 is the caller's, whose back chain
// holds its caller's frame and, 8 bytes below it, the saved return address.
void Chain(const PPCContext& ctx, char* out, size_t size)
{
    int n = snprintf(out, size, "%08llX", (unsigned long long)ctx.lr);
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 8 && OnStack(sp) && n > 0 && size_t(n) + 10 < size; ++i)
    {
        const uint32_t parent = LoadBE32(sp);
        if (!OnStack(parent) || parent <= sp)
            break;
        const uint32_t ra = LoadBE32(parent - 8);
        if (!InImage(ra))
            break;
        n += snprintf(out + n, size - n, " < %08X", ra);
        sp = parent;
    }
}

uint32_t Word(uint32_t address) { return address >= 0x1000 ? LoadBE32(address) : 0u; }
uint32_t Singleton() { return LoadBE32(kSingleton); }
uint32_t RefCount(uint32_t object) { return object ? Word(object + 4) : 0u; }

void Line(const char* what, const char* phase, PPCContext& ctx, uint32_t a, uint32_t b)
{
    char chain[160];
    Chain(ctx, chain, sizeof chain);
    Log("dnet: %s %s (r3 %08X r4 %08X) singleton %08X ref %08X chain %s r13 %08X tid %lu", what, phase, a, b, Singleton(), RefCount(Singleton()), chain,
        ctx.r13.u32, GetCurrentThreadId());
}
void Ret(const char* what, PPCContext& ctx, uint32_t result)
{
    Log("dnet: %s returned %08X singleton %08X ref %08X r13 %08X tid %lu", what, result, Singleton(), RefCount(Singleton()), ctx.r13.u32,
        GetCurrentThreadId());
}
} // namespace

// Called by GUEST_EXPORT after an import returns: the TLS and kernel-object calls made while the singleton is being
// created, and the stack's start and cleanup calls.
void TraceDnetImport(const char* name, PPCContext& ctx)
{
    if (t_createDepth > 0 && (strcmp(name, "KeTlsAlloc") == 0 || strcmp(name, "NtCreateEvent") == 0 || strcmp(name, "NtCreateSemaphore") == 0 ||
                              strcmp(name, "KeInitializeSemaphore") == 0))
        Log("dnet:   in sub_82CE31C0: %s(r3 %08X r4 %08X r5 %08X r6 %08X) = %08X (lr %08llX)", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            ctx.r3.u32, (unsigned long long)ctx.lr);
    else if (strcmp(name, "NetDll_XNetStartup") == 0 || strcmp(name, "NetDll_WSAStartup") == 0 || strcmp(name, "NetDll_XNetCleanup") == 0 ||
             strcmp(name, "NetDll_WSACleanup") == 0)
    {
        char chain[160];
        Chain(ctx, chain, sizeof chain);
        Log("dnet: %s(r3 %08X r4 %08X r5 %08X) = %08X singleton %08X ref %08X chain %s r13 %08X tid %lu", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r3.u32,
            Singleton(), RefCount(Singleton()), chain, ctx.r13.u32, GetCurrentThreadId());
    }
}

// The get-or-create: refcount before and after, and *out (r3) is read again on return: what the caller got.
PPC_FUNC_IMPL(__imp__sub_82CE31C0);
PPC_FUNC(sub_82CE31C0)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CE31C0(ctx, base);
        return;
    }
    const uint32_t out = ctx.r3.u32;
    Line("get-or-create sub_82CE31C0", "enter", ctx, out, ctx.r4.u32);
    ++t_createDepth;
    __imp__sub_82CE31C0(ctx, base);
    --t_createDepth;
    Log("dnet: get-or-create sub_82CE31C0 returned %08X, *out %08X singleton %08X ref %08X r13 %08X tid %lu", ctx.r3.u32, out ? Word(out) : 0u,
        Singleton(), RefCount(Singleton()), ctx.r13.u32, GetCurrentThreadId());
}

// The singleton's Release: r3 is the object; the refcount at +4 is decremented and the destroy runs at 0.
PPC_FUNC_IMPL(__imp__sub_82CE35A0);
PPC_FUNC(sub_82CE35A0)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CE35A0(ctx, base);
        return;
    }
    const uint32_t object = ctx.r3.u32;
    Log("dnet: Release sub_82CE35A0 object %08X ref before %08X", object, RefCount(object));
    Line("Release sub_82CE35A0", "enter", ctx, object, ctx.r4.u32);
    __imp__sub_82CE35A0(ctx, base);
    Log("dnet: Release sub_82CE35A0 returned %08X (new refcount) object %08X ref after %08X singleton %08X", ctx.r3.u32, object, RefCount(object), Singleton());
}

// The destroy: zeroes the global and tears the object down.
PPC_FUNC_IMPL(__imp__sub_82CE34A0);
PPC_FUNC(sub_82CE34A0)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CE34A0(ctx, base);
        return;
    }
    Line("DESTROY sub_82CE34A0", "enter", ctx, ctx.r3.u32, ctx.r4.u32);
    __imp__sub_82CE34A0(ctx, base);
    Ret("DESTROY sub_82CE34A0", ctx, ctx.r3.u32);
}

// The DNET object's first virtual call (vtable slot 3 of every socket-type vtable): this, its vtable and +164.
PPC_FUNC_IMPL(__imp__sub_82CD0808);
PPC_FUNC(sub_82CD0808)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CD0808(ctx, base);
        return;
    }
    const uint32_t self = ctx.r3.u32;
    Log("dnet: DNET slot3 sub_82CD0808 enter this %08X vtable %08X +164 %08X +44 %08X r4 %08X r5 %08X r6 %08X lr %08llX singleton %08X ref %08X tid %lu", self,
        Word(self), Word(self + 164), Word(self + 44), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (unsigned long long)ctx.lr, Singleton(), RefCount(Singleton()),
        GetCurrentThreadId());
    __imp__sub_82CD0808(ctx, base);
    Ret("DNET slot3 sub_82CD0808", ctx, ctx.r3.u32);
}

// What the DNET object copies from the global: the target (this+164) and the value copied.
PPC_FUNC_IMPL(__imp__sub_82CDF870);
PPC_FUNC(sub_82CDF870)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CDF870(ctx, base);
        return;
    }
    const uint32_t target = ctx.r3.u32;
    const uint64_t lr = ctx.lr;
    __imp__sub_82CDF870(ctx, base);
    Log("dnet: copy sub_82CDF870 target %08X (object %08X) copied %08X from global %08X lr %08llX tid %lu", target, target - 164, Word(target), Singleton(),
        (unsigned long long)lr, GetCurrentThreadId());
}

// The cleanup that clears +164.
PPC_FUNC_IMPL(__imp__sub_82CBC8F0);
PPC_FUNC(sub_82CBC8F0)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CBC8F0(ctx, base);
        return;
    }
    const uint32_t object = ctx.r3.u32;
    Log("dnet: cleanup sub_82CBC8F0 object %08X +164 %08X", object, Word(object + 164));
    Line("cleanup sub_82CBC8F0", "enter", ctx, object, ctx.r4.u32);
    __imp__sub_82CBC8F0(ctx, base);
}

#define DNET_TRACE(addr, what)                              \
    PPC_FUNC_IMPL(__imp__sub_##addr);                       \
    PPC_FUNC(sub_##addr)                                    \
    {                                                       \
        if (!g_traceDnet)                                   \
        {                                                   \
            __imp__sub_##addr(ctx, base);                   \
            return;                                         \
        }                                                   \
        Line(what, "enter", ctx, ctx.r3.u32, ctx.r4.u32);   \
        __imp__sub_##addr(ctx, base);                       \
        Ret(what, ctx, ctx.r3.u32);                         \
    }

DNET_TRACE(82CC30D8, "sub_82CC30D8")
DNET_TRACE(82CEAE58, "sub_82CEAE58")
DNET_TRACE(82CE35F8, "acquire sub_82CE35F8")
DNET_TRACE(82CE36B0, "DESTROY-unconditional sub_82CE36B0")
DNET_TRACE(82CBD418, "stack init/shutdown sub_82CBD418")
DNET_TRACE(82CBD550, "stack shutdown sub_82CBD550")
DNET_TRACE(8275DAC0, "kernel-object create sub_8275DAC0")
DNET_TRACE(82CBE348, "DNET constructor sub_82CBE348")
DNET_TRACE(82CD9870, "init struct sub_82CD9870")
DNET_TRACE(82CD98A8, "cleanup struct sub_82CD98A8")
DNET_TRACE(82A6D330, "title class ctor sub_82A6D330")
DNET_TRACE(826BB628, "sub_826BB628")
DNET_TRACE(826BE850, "sub_826BE850")