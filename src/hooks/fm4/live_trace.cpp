// FM4_TRACE_XAM=1: log the title's Live-feature gates ("Feature unavailable" dialogs and the checks that pick
// them) with the guest return address, so a dialog that appears without any request can be traced to the check.
#include "kernel/imports.h"
#include "log.h"
#include "memory.h"

PPC_FUNC_IMPL(__imp__sub_825208C8);
PPC_FUNC(sub_825208C8) // (this, reason, callback): 1 gold, 2 banned, 3 disabled, 4 LSP disconnected, 5 not Live enabled, 6 cannot access Live
{
    if (g_traceXam)
        Log("live gate: Feature-unavailable dialog reason %u (this %08X, lr %08llX)", ctx.r4.u32, ctx.r3.u32, (unsigned long long)ctx.lr);
    __imp__sub_825208C8(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82520B20);
PPC_FUNC(sub_82520B20) // the LSP "feature unavailable" dialog
{
    if (g_traceXam)
        Log("live gate: LSP feature-unavailable dialog (lr %08llX)", (unsigned long long)ctx.lr);
    __imp__sub_82520B20(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_824C4F18);
PPC_FUNC(sub_824C4F18) // the share/upload gate: 0 = fine, else the dialog reason
{
    __imp__sub_824C4F18(ctx, base);
    if (g_traceXam)
        Log("live gate: share gate sub_824C4F18 -> %u (lr %08llX)", ctx.r3.u32, (unsigned long long)ctx.lr);
}

PPC_FUNC_IMPL(__imp__sub_8243C3C0);
PPC_FUNC(sub_8243C3C0) // CPlayer: has a Live-signed-in profile
{
    const uint32_t player = ctx.r3.u32;
    __imp__sub_8243C3C0(ctx, base);
    if (g_traceXam)
    {
        const uint32_t profile = player ? LoadBE32(player + 0x94) : 0;
        Log("live gate: CPlayer %08X profile %08X profile+0x1C %u -> %u (lr %08llX)", player, profile, profile ? LoadBE32(profile + 0x1C) : 0u, ctx.r3.u32,
            (unsigned long long)ctx.lr);
    }
}

PPC_FUNC_IMPL(__imp__sub_8294DD08);
PPC_FUNC(sub_8294DD08) // permission lookup (mgr, user, feature id) -> 0 allowed, else a reason
{
    const uint32_t feature = ctx.r5.u32;
    __imp__sub_8294DD08(ctx, base);
    if (g_traceXam)
        Log("live gate: permission lookup feature %u -> %u (lr %08llX)", feature, ctx.r3.u32, (unsigned long long)ctx.lr);
}

// The other builders of the "cannot access / connect to Live" dialogs and the upload result popups.
#define LIVE_TRACE_ENTRY(addr, what)                                                                     \
    PPC_FUNC_IMPL(__imp__sub_##addr);                                                                    \
    PPC_FUNC(sub_##addr)                                                                                 \
    {                                                                                                    \
        if (g_traceXam)                                                                                  \
            Log("live gate: " what " (r3 %08X r4 %08X lr %08llX)", ctx.r3.u32, ctx.r4.u32, (unsigned long long)ctx.lr); \
        __imp__sub_##addr(ctx, base);                                                                    \
    }

LIVE_TRACE_ENTRY(825EC5A0, "Cannot_Access_Live popup sub_825EC5A0")
LIVE_TRACE_ENTRY(82520CE0, "parental/locked-privilege popup sub_82520CE0")
LIVE_TRACE_ENTRY(82520DC0, "Not_Connected_To_Live popup sub_82520DC0")
LIVE_TRACE_ENTRY(825F1E38, "tuning upload confirm sub_825F1E38")
LIVE_TRACE_ENTRY(825F1F18, "tuning upload result sub_825F1F18")
LIVE_TRACE_ENTRY(825F20A8, "tuning upload sub_825F20A8")
LIVE_TRACE_ENTRY(82622C08, "tuning upload sub_82622C08")
LIVE_TRACE_ENTRY(825BEFC0, "upload row handler sub_825BEFC0")
LIVE_TRACE_ENTRY(826B50D8, "storefront upload manager sub_826B50D8")
LIVE_TRACE_ENTRY(825A8A98, "multiplayer join-failure popup sub_825A8A98")

// The winnings / predicted payout formula (event details and the race result): length x laps x rate x bounty x scalar.
// Log what it is called with and what it returns, to see which factor makes a Rival payout 0.
PPC_FUNC_IMPL(__imp__sub_824872F8);
PPC_FUNC(sub_824872F8)
{
    const uint64_t lr = ctx.lr;
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    const double f1 = ctx.f1.f64, f2 = ctx.f2.f64;
    const uint32_t bountyHolder = LoadBE32(0x82DC386C);
    const int32_t bounty = bountyHolder ? int32_t(LoadBE32(bountyHolder + 0x384)) : 0;
    __imp__sub_824872F8(ctx, base);
    if (g_traceXam)
        Log("winnings: sub_824872F8(r3 %08X r4 %08X r5 %08X f1 %g f2 %g, bounty %d) = r3 %08X f1 %g (lr %08llX)", r3, r4, r5, f1, f2, bounty,
            ctx.r3.u32, ctx.f1.f64, (unsigned long long)lr);
}
