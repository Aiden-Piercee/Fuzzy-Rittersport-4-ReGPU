// The Xbox 360 wheel's force feedback: the kernel's XInputdFF* calls (xboxkrnl ordinals 0x282-0x289)
// and the XAM calls around them. FM4 doesn't import them: its XInputFF library looks them up at start
// with XexGetModuleHandle("xboxkrnl.exe") and XexGetProcedureAddress, eight times, as FH1's does
// (src/input/wheel/wheel_ff.h). Those two modules aren't loaded images here, so XexGetModuleHandle
// gives them handles of their own (HostModuleHandle) and XexGetProcedureAddress the address of a host
// function for the ordinals below (HostProcedure, IndirectAddHost); any other ordinal is logged and not
// found, as before. The effects play on the racing wheels (src/input/wheel/wheel_system.cpp).
//
// Ported from Forza Horizon 1: ReImagined (SDK patch 0101, xboxkrnl_hid.cpp and xam_input.cpp; BSD
// 3-Clause, see NOTICE-wheel.md). With an XOVERLAPPED a call completes at once and returns
// STATUS_PENDING, which the title's XInputFF library turns into ERROR_IO_PENDING and then reads the
// result from the overlapped.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "indirect.h"
#include "input/wheel/wheel_ff.h"
#include "input/wheel/wheel_system.h"
#include "kernel/imports.h"

wheel::System& GameWheels(); // imports_xam.cpp

namespace
{
constexpr uint32_t kStatusDeviceNotConnected = 0xC000009D;
constexpr uint32_t kKernelHandle = 0xFFFE0000, kXamHandle = 0xFFFE0010;

uint32_t FfDone(bool ok, uint32_t overlapped)
{
    if (overlapped)
    {
        CompleteOverlapped(overlapped, ok ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED);
        if (ok)
            return X_STATUS_PENDING;
    }
    return ok ? X_STATUS_SUCCESS : kStatusDeviceNotConnected;
}

void Once(const char* name)
{
    // The first call of each, for the log.
    static std::mutex mutex;
    static std::vector<const char*> seen;
    std::lock_guard l(mutex);
    if (std::find(seen.begin(), seen.end(), name) == seen.end())
    {
        seen.push_back(name);
        Log("Wheel: the game calls %s", name);
    }
}

// (handle, info*): the second word is how many effects the device plays at once (the game needs 5).
void FfGetDeviceInfo(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFGetDeviceInfo");
    const uint32_t info = ctx.r4.u32;
    if (!info)
        return Return(ctx, X_STATUS_INVALID_PARAMETER);
    if (!GameWheels().FfAvailable())
        return Return(ctx, kStatusDeviceNotConnected);
    StoreBE32(info, 0);
    StoreBE32(info + 4, wheel::kFfMaxBlocks);
    Return(ctx, X_STATUS_SUCCESS);
}

// (handle, effects, count, overlapped): `count` effects of 0x74 bytes, each naming its block.
void FfSetEffect(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFSetEffect");
    const uint32_t effects = ctx.r4.u32, count = ctx.r5.u32;
    if (!effects || !count || count > wheel::kFfMaxBlocks)
        return Return(ctx, X_STATUS_INVALID_PARAMETER);
    bool ok = true;
    for (uint32_t i = 0; i < count; i++)
        ok = GameWheels().FfSetEffect(wheel::DecodeFfEffect(GuestPtr<uint8_t>(effects + i * 4 * uint32_t(wheel::kFfEffectWords)))) && ok;
    Return(ctx, FfDone(ok, ctx.r6.u32));
}

// (handle, effect, flags, overlapped): flags say what changed (0x400 the type's parameters, 0x100 the gain).
void FfUpdateEffect(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFUpdateEffect");
    if (!ctx.r4.u32)
        return Return(ctx, X_STATUS_INVALID_PARAMETER);
    Return(ctx, FfDone(GameWheels().FfSetEffect(wheel::DecodeFfEffect(GuestPtr<uint8_t>(ctx.r4.u32))), ctx.r6.u32));
}

// (handle, operations, count, loop count, overlapped): `count` pairs of (block, operation).
void FfEffectOperation(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFEffectOperation");
    const uint32_t ops = ctx.r4.u32, count = ctx.r5.u32;
    if (!ops || !count || count > wheel::kFfMaxBlocks)
        return Return(ctx, X_STATUS_INVALID_PARAMETER);
    bool ok = true;
    for (uint32_t i = 0; i < count; i++)
        ok = GameWheels().FfOperate(LoadBE32(ops + 8 * i), LoadBE32(ops + 8 * i + 4)) && ok;
    Return(ctx, FfDone(ok, ctx.r7.u32));
}

// (handle, command, overlapped): HID PID device control (the game sends 3, stop all).
void FfDeviceControl(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFDeviceControl");
    Return(ctx, FfDone(GameWheels().FfControl(ctx.r4.u32), ctx.r5.u32));
}

// (handle, gain 0-255, overlapped)
void FfSetDeviceGain(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFSetDeviceGain");
    Return(ctx, FfDone(GameWheels().FfSetGain(ctx.r4.u32), ctx.r5.u32));
}

// Every call completes at once.
void FfCancelIo(PPCContext& __restrict ctx, uint8_t*)
{
    Return(ctx, X_STATUS_SUCCESS);
}

// (handle, XINPUT_VIBRATION*, overlapped): the two motor speeds, big-endian.
void FfSetRumble(PPCContext& __restrict ctx, uint8_t*)
{
    Once("XInputdFFSetRumble");
    const uint32_t v = ctx.r4.u32;
    if (!v)
        return Return(ctx, X_STATUS_INVALID_PARAMETER);
    const uint16_t left = __builtin_bswap16(*GuestPtr<uint16_t>(v)), right = __builtin_bswap16(*GuestPtr<uint16_t>(v + 2));
    Return(ctx, FfDone(GameWheels().FfRumble(left, right), ctx.r5.u32));
}

// (user, level*): the profile's vibration setting; the XInputFF library sends no rumble for 0.
void XamVibrationLevel(PPCContext& __restrict ctx, uint8_t*)
{
    if (!ctx.r4.u32)
        return Return(ctx, 160); // ERROR_BAD_ARGUMENTS
    StoreBE32(ctx.r4.u32, 1);
    Return(ctx, X_ERROR_SUCCESS);
}

struct Name
{
    uint32_t ordinal;
    const char* name;
};
#define XE_EXPORT(module, ordinal, name, kind) { ordinal, #name } // each line of the tables ends with its comma
const Name kKernelNames[] = {
#include "../../thirdparty/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc"
};
const Name kXamNames[] = {
#include "../../thirdparty/XenonRecomp/XenonUtils/xbox/xam_table.inc"
};
#undef XE_EXPORT

const char* NameOf(const Name* table, size_t count, uint32_t ordinal)
{
    for (size_t i = 0; i < count; i++)
        if (table[i].ordinal == ordinal)
            return table[i].name;
    return "?";
}

struct Proc
{
    uint32_t handle;
    const char* name;
    PPCHostFunc* func;
};
} // namespace

// The XAM input calls the game also imports, for a lookup by ordinal.
PPC_EXTERN_FUNC(__imp__XamUserGetDeviceContext);
PPC_EXTERN_FUNC(__imp__XamInputGetCapabilities);
PPC_EXTERN_FUNC(__imp__XamInputGetCapabilitiesEx);
PPC_EXTERN_FUNC(__imp__XamInputGetState);
PPC_EXTERN_FUNC(__imp__XamInputSetState);

bool HostModuleHandle(const std::string& name, uint32_t* handle)
{
    std::string n = name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(tolower(c)); });
    if (n == "xboxkrnl.exe")
        *handle = kKernelHandle;
    else if (n == "xam.xex")
        *handle = kXamHandle;
    else
        return false;
    return true;
}

bool HostProcedure(uint32_t handle, uint32_t ordinal, uint32_t* address)
{
    if (handle != kKernelHandle && handle != kXamHandle)
        return false;
    const bool kernel = handle == kKernelHandle;
    const char* name = kernel ? NameOf(kKernelNames, std::size(kKernelNames), ordinal) : NameOf(kXamNames, std::size(kXamNames), ordinal);
    static const Proc procs[] = {
        { kKernelHandle, "XInputdFFGetDeviceInfo", FfGetDeviceInfo },
        { kKernelHandle, "XInputdFFSetEffect", FfSetEffect },
        { kKernelHandle, "XInputdFFUpdateEffect", FfUpdateEffect },
        { kKernelHandle, "XInputdFFEffectOperation", FfEffectOperation },
        { kKernelHandle, "XInputdFFDeviceControl", FfDeviceControl },
        { kKernelHandle, "XInputdFFSetDeviceGain", FfSetDeviceGain },
        { kKernelHandle, "XInputdFFCancelIo", FfCancelIo },
        { kKernelHandle, "XInputdFFSetRumble", FfSetRumble },
        { kXamHandle, "XamInputGetUserVibrationLevel", XamVibrationLevel },
        { kXamHandle, "XamUserGetDeviceContext", __imp__XamUserGetDeviceContext },
        { kXamHandle, "XamInputGetCapabilities", __imp__XamInputGetCapabilities },
        { kXamHandle, "XamInputGetCapabilitiesEx", __imp__XamInputGetCapabilitiesEx },
        { kXamHandle, "XamInputGetState", __imp__XamInputGetState },
        { kXamHandle, "XamInputSetState", __imp__XamInputSetState },
    };
    for (const Proc& p : procs)
        if (p.handle == handle && strcmp(p.name, name) == 0)
        {
            *address = IndirectAddHost(p.func);
            Log("XexGetProcedureAddress(%s, %03X): %s, at %08X", kernel ? "xboxkrnl.exe" : "xam.xex", ordinal, name, *address);
            return *address != 0;
        }
    Log("XexGetProcedureAddress(%s, %03X): %s is not available", kernel ? "xboxkrnl.exe" : "xam.xex", ordinal, name);
    *address = 0;
    return true; // handled: not found
}
