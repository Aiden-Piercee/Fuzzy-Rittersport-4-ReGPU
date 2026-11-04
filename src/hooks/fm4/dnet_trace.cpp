// FM4_TRACE_DNET=1: trace the title's network-manager singleton (the guest word at 0x82E9D3F0) and the objects that
// copy it. sub_82CE31C0 is its get-or-create (callers sub_82CC30D8, sub_82CEAE58, sub_82CE35F8 release it); the "DNET"
// object built by sub_82CBE348 copies the word into its +164 without creating it, and a null there ends in an indirect
// call to 0 at lr 82CD08C0. Each line has the guest thread register r13, the host thread id and the word's value.
// Off by default; read-only (the hooks call the original and only log).
#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

namespace
{
const bool g_traceDnet = getenv("FM4_TRACE_DNET") != nullptr;
constexpr uint32_t kSingleton = 0x82E9D3F0;
thread_local int t_createDepth; // inside sub_82CE31C0 on this thread

void Line(const char* what, uint32_t a, uint32_t b, uint64_t lr, PPCContext& ctx)
{
    Log("dnet: %s (r3 %08X r4 %08X lr %08llX) singleton %08X r13 %08X tid %lu", what, a, b, (unsigned long long)lr, LoadBE32(kSingleton), ctx.r13.u32,
        GetCurrentThreadId());
}
void Ret(const char* what, uint32_t r3, uint64_t lr, PPCContext& ctx)
{
    Log("dnet: %s returned %08X (lr %08llX) singleton %08X r13 %08X tid %lu", what, r3, (unsigned long long)lr, LoadBE32(kSingleton), ctx.r13.u32,
        GetCurrentThreadId());
}
} // namespace

// Called by GUEST_EXPORT after an import returns: the TLS and kernel-object calls made while the singleton is being created.
void TraceDnetImport(const char* name, PPCContext& ctx)
{
    if (strcmp(name, "KeTlsAlloc") == 0 || strcmp(name, "NtCreateEvent") == 0 || strcmp(name, "NtCreateSemaphore") == 0 ||
        strcmp(name, "KeInitializeSemaphore") == 0)
        Log("dnet:   in sub_82CE31C0: %s(r3 %08X r4 %08X r5 %08X r6 %08X) = %08X (lr %08llX)", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            ctx.r3.u32, (unsigned long long)ctx.lr);
}
bool TraceDnetInside() { return g_traceDnet && t_createDepth > 0; }

// The out parameter of the get-or-create (r3) is read again on return: what the caller got.
PPC_FUNC_IMPL(__imp__sub_82CE31C0);
PPC_FUNC(sub_82CE31C0)
{
    if (!g_traceDnet)
    {
        __imp__sub_82CE31C0(ctx, base);
        return;
    }
    const uint32_t out = ctx.r3.u32;
    const uint64_t lr = ctx.lr;
    Line("get-or-create sub_82CE31C0 enter", out, ctx.r4.u32, lr, ctx);
    ++t_createDepth;
    __imp__sub_82CE31C0(ctx, base);
    --t_createDepth;
    Log("dnet: get-or-create sub_82CE31C0 returned %08X, *out %08X (lr %08llX) singleton %08X r13 %08X tid %lu", ctx.r3.u32, out ? LoadBE32(out) : 0u,
        (unsigned long long)lr, LoadBE32(kSingleton), ctx.r13.u32, GetCurrentThreadId());
}

#define DNET_TRACE(addr, what)                                         \
    PPC_FUNC_IMPL(__imp__sub_##addr);                                  \
    PPC_FUNC(sub_##addr)                                               \
    {                                                                  \
        if (!g_traceDnet)                                              \
        {                                                              \
            __imp__sub_##addr(ctx, base);                              \
            return;                                                    \
        }                                                              \
        const uint64_t lr = ctx.lr;                                    \
        Line(what " enter", ctx.r3.u32, ctx.r4.u32, lr, ctx);          \
        __imp__sub_##addr(ctx, base);                                  \
        Ret(what, ctx.r3.u32, lr, ctx);                                \
    }

DNET_TRACE(82CC30D8, "sub_82CC30D8")
DNET_TRACE(82CEAE58, "sub_82CEAE58")
DNET_TRACE(82CE35F8, "release sub_82CE35F8")
DNET_TRACE(8275DAC0, "kernel-object create sub_8275DAC0")
DNET_TRACE(82CBE348, "DNET constructor sub_82CBE348")
