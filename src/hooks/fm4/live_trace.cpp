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
