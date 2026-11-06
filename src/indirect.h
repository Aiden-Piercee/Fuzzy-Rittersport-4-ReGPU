#pragma once
#include <cstdint>

// Builds the indirect-call table (g_indirectTable, ppc_hooks.h) from every recompiled
// image in g_recompiledModules (recomp_modules.h). Call once before running guest code.
void IndirectInit();
// The host function for a guest code address, or null if none was recompiled there.
struct PPCContext;
using PPCHostFunc = void(PPCContext&, uint8_t*);
PPCHostFunc* IndirectFind(uint32_t address);
// A guest address the game can call (through a function pointer) that runs `func`: kernel exports the
// game looks up at run time (XexGetProcedureAddress) and no image imports (imports_ff.cpp). The same
// function gets the same address. 0 when the table is not built yet or the slots are used up.
uint32_t IndirectAddHost(PPCHostFunc* func);
