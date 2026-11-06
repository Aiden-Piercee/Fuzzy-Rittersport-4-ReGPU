// The indirect-call table shared by all recompiled images (see ppc_hooks.h). It lives in
// host memory outside the guest's 4 GB, so module images and their tables cannot overlap.
#include "indirect.h"

#include <mutex>

#include <windows.h>

#include "ppc_hooks.h"
#include <ppc_config.h>
#include <ppc_context.h>

#include "log.h"
#include "recomp_modules.h"

PPCHostFunc** g_indirectTable = nullptr;

void PPCIndirectMiss(uint32_t address)
{
    Fatal("indirect call to %08X: no recompiled function there", address);
}

void IndirectInit()
{
    // Reserve one pointer per instruction over the whole range, then commit only the
    // pages that cover each image's code.
    const size_t entries = (kIndirectHigh - kIndirectLow) / 4;
    auto* table = static_cast<PPCHostFunc**>(VirtualAlloc(nullptr, entries * sizeof(PPCHostFunc*), MEM_RESERVE, PAGE_NOACCESS));
    if (!table)
        Fatal("indirect calls: reserving the table failed (error %lu)", GetLastError());
    size_t mapped = 0;
    for (size_t i = 0; i < g_recompiledModuleCount; i++)
    {
        const RecompiledModule& m = g_recompiledModules[i];
        if (m.codeBase < kIndirectLow || m.codeBase + m.codeSize > kIndirectHigh || m.codeSize == 0)
            Fatal("indirect calls: %s code %08X+%X is outside %08X-%08X", m.guestPath, m.codeBase, m.codeSize, kIndirectLow, kIndirectHigh);
        PPCHostFunc** first = table + (m.codeBase - kIndirectLow) / 4;
        if (!VirtualAlloc(first, size_t(m.codeSize / 4) * sizeof(PPCHostFunc*), MEM_COMMIT, PAGE_READWRITE))
            Fatal("indirect calls: committing the table for %s failed (error %lu)", m.guestPath, GetLastError());
        size_t count = 0;
        for (const PPCFuncMapping* f = m.mappings; f->host; f++, count++)
        {
            if (f->guest < m.codeBase || f->guest >= m.codeBase + m.codeSize)
                Fatal("indirect calls: %s maps %08llX outside its code", m.guestPath, uint64_t(f->guest));
            table[(f->guest - kIndirectLow) / 4] = f->host;
        }
        Log("indirect calls: %s, %zu functions", m.guestPath, count);
        mapped += count;
    }
    g_indirectTable = table;
    Log("indirect calls: %zu functions in %zu images", mapped, g_recompiledModuleCount);
}

// Host functions the game calls by address sit in the last 4 KB of the table's range, above every
// image (IndirectInit checks that none reaches it). The addresses are call targets only: nothing is
// mapped or executed there.
constexpr uint32_t kHostLow = kIndirectHigh - 0x1000;

uint32_t IndirectAddHost(PPCHostFunc* func)
{
    static std::mutex mutex;
    static uint32_t next = kHostLow;
    std::lock_guard lock(mutex);
    if (!g_indirectTable)
        return 0;
    for (size_t i = 0; i < g_recompiledModuleCount; i++)
        if (g_recompiledModules[i].codeBase + g_recompiledModules[i].codeSize > kHostLow)
            Fatal("indirect calls: %s reaches the host function slots at %08X", g_recompiledModules[i].guestPath, kHostLow);
    PPCHostFunc** first = g_indirectTable + (kHostLow - kIndirectLow) / 4;
    if (next == kHostLow && !VirtualAlloc(first, 0x1000 / 4 * sizeof(PPCHostFunc*), MEM_COMMIT, PAGE_READWRITE))
        Fatal("indirect calls: committing the host function slots failed (error %lu)", GetLastError());
    for (uint32_t a = kHostLow; a < next; a += 4)
        if (g_indirectTable[(a - kIndirectLow) / 4] == func)
            return a;
    if (next >= kIndirectHigh)
        return 0;
    g_indirectTable[(next - kIndirectLow) / 4] = func;
    next += 4;
    return next - 4;
}

PPCHostFunc* IndirectFind(uint32_t address)
{
    // Only image code has committed table pages; the hot path (PPCLookupIndirect) skips
    // this check, so a call to any other address faults in the table instead.
    for (size_t i = 0; i < g_recompiledModuleCount; i++)
    {
        const RecompiledModule& m = g_recompiledModules[i];
        if (address - m.codeBase < m.codeSize)
            return g_indirectTable[(address - kIndirectLow) / 4];
    }
    return nullptr;
}
