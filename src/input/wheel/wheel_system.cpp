// Racing wheels through DirectInput (see wheel_system.h). The device and control logic is FH1's
// wheel driver (wheel_logic.h); this file is its DirectInput side and its state computation, ported
// from FH1's wheel_input_driver.cpp (ComputeStateLocked, RescanLocked).
//
// Axes are numbered in HID usage order among those the device has: X, Y, Z, Rx, Ry, Rz, slider 1,
// slider 2 (as SDL numbers them, so FH1's device tables apply), each read as -32768..32767; buttons
// from 0; up to four hats. Devices are opened background and non-exclusive: the wheel's own driver
// keeps its settings (rotation, autocentre), and no force feedback is played.
#include "input/wheel/wheel_system.h"
#include "input/wheel/wheel_ff.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

namespace wheel
{

namespace
{
LogFn g_log = nullptr;

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_log(line);
}

double NowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Utf8(const wchar_t* w)
{
    char buf[512];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf, size_t(n - 1)) : std::string();
}

std::string GuidString(const GUID& g)
{
    char b[40];
    snprintf(b, sizeof(b), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0],
             g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

const DWORD kAxisOffsets[8] = { DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1) };
constexpr const char* kAxisNames[8] = { "X", "Y", "Z", "Rx", "Ry", "Rz", "Slider 1", "Slider 2" };

uint8_t PovBits(DWORD pov)
{
    if (LOWORD(pov) == 0xFFFF)
        return 0;
    const DWORD a = pov % 36000;
    uint8_t b = 0;
    if (a > 27000 || a < 9000) b |= kHatUp;
    if (a > 0 && a < 18000) b |= kHatRight;
    if (a > 9000 && a < 27000) b |= kHatDown;
    if (a > 18000) b |= kHatLeft;
    return b;
}

const char* RoleName(AuxRole r)
{
    return r == AuxRole::Pedals ? "pedals" : r == AuxRole::Shifter ? "shifter" : r == AuxRole::Handbrake ? "handbrake" : "";
}
} // namespace

void SetLog(LogFn fn) { g_log = fn; }

// A wheel's force feedback: a second DirectInput device on it, exclusive (DirectInput plays effects
// only for an exclusive owner; the input stays on the non-exclusive one), and the game's effect blocks
// on it. Opened when the game first sends force feedback, so the wheel keeps its own autocentre until
// then. Effects use one axis (the first force feedback actuator, normally the steering) with a
// cartesian direction of 0, as SDL's DirectInput backend does, which FH1's sign was tested with.
struct Haptic
{
    IDirectInputDevice8W* dev = nullptr;
    DWORD axis = DIJOFS_X;
    IDirectInputEffect* fx[kFfMaxBlocks] = {};
    GUID fxGuid[kFfMaxBlocks] = {};
    bool running[kFfMaxBlocks] = {};
    std::string sent[kFfMaxBlocks]; // the parameters last sent, to skip repeats (the game updates every frame)
    LONG sentMagnitude[kFfMaxBlocks] = {}; // a constant force's last magnitude (the FM2 playback)
    // The rumble: fine (road, engine) or classic (one sine).
    IDirectInputEffect* sine[3] = {};
    DWORD sineMagnitude[3] = { DWORD(-1), DWORD(-1), DWORD(-1) };
    bool sineRunning[3] = {};
    bool sineFailed = false;
    bool failed = false; // could not be opened: don't try every frame

    ~Haptic()
    {
        for (auto*& f : fx)
            if (f) { f->Stop(); f->Unload(); f->Release(); f = nullptr; }
        for (auto*& f : sine)
            if (f) { f->Stop(); f->Unload(); f->Release(); f = nullptr; }
        if (dev)
        {
            dev->SendForceFeedbackCommand(DISFFC_STOPALL);
            dev->Unacquire();
            dev->Release();
        }
    }
};

struct Dev
{
    GUID instance{};
    IDirectInputDevice8W* di = nullptr;
    JoyInfo info;
    bool gamepadType = false;
    bool ffb = false;
    int field[8] = {};    // DIJOYSTATE2 axis (0..7) of each of the device's axes
    int numAxes = 0, numButtons = 0, numHats = 0;
    std::vector<std::string> axisNames;
    std::vector<int16_t> rest;   // where each axis sat when the device was opened
    std::vector<uint8_t> armed;  // pedal axes seen at rest since then
    bool restPending = false;    // every axis read 0 when opened: rest taken from the first report
    RawView raw;
    bool lost = false;
    uint64_t serial = 0;
    std::unique_ptr<Haptic> haptic; // force feedback, once the game drives it
};

struct WheelState
{
    int gear = kGearNeutral;
    GearRequest request;
    bool titleCaps = false;
};

struct System::Impl
{
    std::mutex mutex;
    Config cfg;
    std::vector<std::unique_ptr<Dev>> devs;
    std::vector<int> order; // the wheels, by index into devs
    AuxPick pick;
    std::unordered_map<uint64_t, WheelState> states;
    uint64_t nextSerial = 1;
    std::atomic<uint32_t> listSerial{ 1 };
    double lastPump = 0;
    IDirectInput8W* di = nullptr;
    HWND hwnd = nullptr;
    bool started = false;
    std::mutex startMutex;
    std::condition_variable startCv;
    bool firstScan = false;
    std::string loggedPrimary, loggedMissing[4];

    // ---- devices (the scan thread)

    struct Found
    {
        GUID instance, product;
        DWORD type;
        std::string name;
    };

    void Thread()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"fm4-wheel-input";
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"fm4 wheel input", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr)))
        {
            Log("Wheel: DirectInput is not available: no wheel support");
            di = nullptr;
        }
        for (;;)
        {
            if (di)
                Scan();
            {
                std::lock_guard l(startMutex);
                firstScan = true;
            }
            startCv.notify_all();
            const ULONGLONG until = GetTickCount64() + 1000;
            for (ULONGLONG now = GetTickCount64(); now < until; now = GetTickCount64())
            {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(until - now), QS_ALLINPUT);
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
    }

    static BOOL CALLBACK EnumCb(const DIDEVICEINSTANCEW* d, void* ctx)
    {
        static_cast<std::vector<Found>*>(ctx)->push_back({ d->guidInstance, d->guidProduct, d->dwDevType, Utf8(d->tszProductName) });
        return DIENUM_CONTINUE;
    }

    void Scan()
    {
        std::vector<Found> found;
        if (FAILED(di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, &found, DIEDFL_ATTACHEDONLY)))
            return;
        // Open the new ones outside the lock (it can take a while), then swap them in.
        std::vector<std::unique_ptr<Dev>> opened;
        {
            std::lock_guard l(mutex);
            for (const Found& f : found)
            {
                bool have = false;
                for (auto& d : devs)
                    have = have || (IsEqualGUID(d->instance, f.instance) && !d->lost);
                if (have)
                    continue;
                // XInput pads (Microsoft's) stay with the gamepad path.
                const bool pidvid = memcmp(&f.product.Data4[2], "PIDVID", 6) == 0;
                if (pidvid && LOWORD(f.product.Data1) == 0x045E)
                    continue;
                auto d = std::make_unique<Dev>();
                d->instance = f.instance;
                d->info.name = f.name.empty() ? "Joystick" : f.name;
                d->info.guid = GuidString(f.instance);
                d->info.vid = pidvid ? LOWORD(f.product.Data1) : 0;
                d->info.pid = pidvid ? HIWORD(f.product.Data1) : 0;
                d->info.driving = GET_DIDEVICE_TYPE(f.type) == DI8DEVTYPE_DRIVING;
                d->gamepadType = GET_DIDEVICE_TYPE(f.type) == DI8DEVTYPE_GAMEPAD;
                opened.push_back(std::move(d));
            }
        }
        for (auto it = opened.begin(); it != opened.end();)
            it = Open(**it) ? it + 1 : opened.erase(it);

        std::lock_guard l(mutex);
        bool changed = false;
        for (size_t i = 0; i < devs.size();)
        {
            bool present = false;
            for (const Found& f : found)
                present = present || IsEqualGUID(f.instance, devs[i]->instance);
            if (!present || devs[i]->lost)
            {
                Log("Wheel: \"%s\" removed", devs[i]->info.name.c_str());
                states.erase(devs[i]->serial);
                if (devs[i]->di)
                {
                    devs[i]->di->Unacquire();
                    devs[i]->di->Release();
                }
                devs.erase(devs.begin() + i);
                changed = true;
            }
            else
                i++;
        }
        for (auto& d : opened)
        {
            d->serial = nextSerial++;
            devs.push_back(std::move(d));
            changed = true;
        }
        if (changed)
        {
            listSerial++;
            Recompute();
        }
    }

    bool Open(Dev& d)
    {
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(di->CreateDevice(d.instance, &dev, nullptr)))
        {
            Log("Wheel: could not open \"%s\"", d.info.name.c_str());
            return false;
        }
        if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(dev->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
        {
            Log("Wheel: could not set up \"%s\"", d.info.name.c_str());
            dev->Release();
            return false;
        }
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        dev->GetCapabilities(&caps);
        d.ffb = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
        d.numButtons = int(std::min<DWORD>(caps.dwButtons, 128));
        d.numHats = int(std::min<DWORD>(caps.dwPOVs, 4));
        for (int f = 0; f < 8; f++)
        {
            DIDEVICEOBJECTINSTANCEW obj{};
            obj.dwSize = sizeof(obj);
            if (FAILED(dev->GetObjectInfo(&obj, kAxisOffsets[f], DIPH_BYOFFSET)))
                continue;
            DIPROPRANGE range{};
            range.diph.dwSize = sizeof(range);
            range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            range.diph.dwObj = kAxisOffsets[f];
            range.diph.dwHow = DIPH_BYOFFSET;
            range.lMin = -32768;
            range.lMax = 32767;
            dev->SetProperty(DIPROP_RANGE, &range.diph);
            DIPROPDWORD dz{};
            dz.diph.dwSize = sizeof(dz);
            dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dz.diph.dwObj = kAxisOffsets[f];
            dz.diph.dwHow = DIPH_BYOFFSET;
            dz.dwData = 0;
            dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
            d.field[d.numAxes++] = f;
            d.axisNames.push_back(kAxisNames[f]);
        }
        d.di = dev;
        dev->Acquire();
        d.raw.axes.assign(size_t(d.numAxes), 0);
        d.raw.buttons.assign(size_t(d.numButtons), 0);
        d.raw.hats.assign(size_t(d.numHats), 0);
        Sleep(20); // let the first report in
        PollOne(d);
        d.rest = d.raw.axes;
        d.armed.assign(d.rest.size(), 0);
        d.restPending = !FindProfile(d.info.vid, d.info.pid) && RestPending(d.rest);
        const AuxModel* aux = FindAuxModel(d.info.vid, d.info.pid);
        const ThrustmasterModel* tm = FindThrustmaster(d.info.vid, d.info.pid);
        const Profile* prof = FindProfile(d.info.vid, d.info.pid);
        Log("Wheel: \"%s\" GUID(%s) VID(0x%04X) PID(0x%04X) type(%s) axes(%d) buttons(%d) hats(%d) force feedback(%s)%s%s",
            d.info.name.c_str(), d.info.guid.c_str(), d.info.vid, d.info.pid,
            d.info.driving ? "wheel" : d.gamepadType ? "gamepad" : "joystick", d.numAxes, d.numButtons, d.numHats,
            d.ffb ? "yes" : "no", (aux || tm || prof) ? ", known: " : "", aux ? aux->name : tm ? tm->name : prof ? prof->name : "");
        for (int a = 0; a < d.numAxes; a++)
            Log("Wheel:   axis %d (%s) rests at %d", a, d.axisNames[a].c_str(), d.rest[a]);
        if (d.restPending)
            Log("Wheel: every axis of \"%s\" reads 0 (no report yet): where they rest is read from the first report", d.info.name.c_str());
        return true;
    }

    void PollOne(Dev& d)
    {
        if (!d.di)
            return;
        HRESULT hr = d.di->Poll();
        DIJOYSTATE2 js{};
        if (SUCCEEDED(hr) || hr == DI_NOEFFECT)
            hr = d.di->GetDeviceState(sizeof(js), &js);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
        {
            const HRESULT a = d.di->Acquire();
            if (a == DIERR_UNPLUGGED)
                d.lost = true;
            return;
        }
        if (FAILED(hr))
            return;
        const LONG fields[8] = { js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz, js.rglSlider[0], js.rglSlider[1] };
        for (int a = 0; a < d.numAxes; a++)
            d.raw.axes[a] = int16_t(std::clamp<LONG>(fields[d.field[a]], -32768, 32767));
        for (int b = 0; b < d.numButtons; b++)
            d.raw.buttons[b] = (js.rgbButtons[b] & 0x80) ? 1 : 0;
        for (int h = 0; h < d.numHats; h++)
            d.raw.hats[h] = PovBits(js.rgdwPOV[h]);
    }

    // ---- which device is what (under the lock)

    bool WheelLike(const Dev& d) const
    {
        if (cfg.mode == "off" || d.info.vid == 0x045E)
            return false;
        return d.info.driving || NameLooksLikeWheel(d.info.name) || FindThrustmaster(d.info.vid, d.info.pid) ||
               (cfg.mode == "on" && !d.gamepadType);
    }

    void Recompute()
    {
        std::vector<JoyInfo> joys;
        for (auto& d : devs)
            joys.push_back(d->info);
        order.clear();
        pick = AuxPick{};
        if (cfg.mode == "off")
            return;
        pick = PickAuxDevices(joys, cfg.pedalDevice, cfg.shifterDevice, cfg.handbrakeDevice);
        std::vector<bool> like(devs.size()), ffb(devs.size()), aux(devs.size());
        for (size_t i = 0; i < devs.size(); i++)
        {
            like[i] = WheelLike(*devs[i]);
            ffb[i] = devs[i]->ffb;
            aux[i] = pick.IsAux(int(i));
        }
        order = WheelOrder(joys, like, ffb, aux, cfg.device);
        // A device can't be the wheel and its own pedals or shifter at once (wheel_device naming one).
        if (!order.empty())
        {
            if (pick.pedals == order[0]) pick.pedals = -1;
            if (pick.shifter == order[0]) pick.shifter = -1;
            if (pick.handbrake == order[0]) pick.handbrake = -1;
        }
        if (!order.empty() && devs[order[0]]->info.name != loggedPrimary)
        {
            loggedPrimary = devs[order[0]]->info.name;
            const bool named = WheelDeviceMatch(cfg.device, devs[order[0]]->info) > 0;
            Log("Wheel: \"%s\" is the wheel (the first player's; %s)%s", loggedPrimary.c_str(),
                named ? "wheel_device" : devs[order[0]]->ffb ? "it has force feedback" : "the first of them",
                order.size() > 1 ? "; the others are more controllers" : "");
        }
        auto note = [&](int k, const char* key, const std::string& setting, int idx, const char* what) {
            if (idx >= 0)
            {
                if (loggedMissing[k] != devs[idx]->info.name)
                {
                    loggedMissing[k] = devs[idx]->info.name;
                    Log("Wheel: %s \"%s\" is read with the wheel", what, loggedMissing[k].c_str());
                }
            }
            else if (ParseDeviceChoice(setting) == DeviceChoice::Named && loggedMissing[k] != "?" + setting)
            {
                loggedMissing[k] = "?" + setting;
                Log("Wheel: %s \"%s\" is not connected", key, setting.c_str());
            }
            else if (ParseDeviceChoice(setting) != DeviceChoice::Named)
                loggedMissing[k].clear();
        };
        note(0, "wheel_pedal_device", cfg.pedalDevice, pick.pedals, "pedal device");
        note(1, "wheel_shifter_device", cfg.shifterDevice, pick.shifter, "shifter device");
        note(2, "wheel_handbrake_device", cfg.handbrakeDevice, pick.handbrake, "handbrake device");
        if (ParseDeviceChoice(cfg.device) == DeviceChoice::Named &&
            std::none_of(joys.begin(), joys.end(), [&](const JoyInfo& j) { return WheelDeviceMatch(cfg.device, j) > 0; }) &&
            loggedMissing[3] != cfg.device)
        {
            loggedMissing[3] = cfg.device;
            Log("Wheel: wheel_device \"%s\" is not connected", cfg.device.c_str());
        }
    }

    void Pump()
    {
        const double now = NowMs();
        if (now - lastPump < 2.0)
            return;
        lastPump = now;
        for (auto& d : devs)
            PollOne(*d);
    }

    // ---- what each control reads (under the lock)

    struct Ax
    {
        Dev* dev = nullptr;
        int axis = -1;
        bool known = false; // a known wheel's pedal: rests at the maximum
    };
    struct Plan
    {
        Dev* wheel = nullptr;
        Dev *pedals = nullptr, *shifterDev = nullptr, *hbDev = nullptr;
        Ax steer, pedal[3], handbrake;
        bool combined = false;
        std::map<std::string, int> buttons, overrides;
        ShifterMap shifter;
        Dev* shifterOn = nullptr;
    };

    Dev* At(int i) { return i >= 0 && i < int(devs.size()) ? devs[i].get() : nullptr; }

    Plan MakePlan(Dev& w, bool primary)
    {
        Plan p;
        p.wheel = &w;
        if (primary)
        {
            p.pedals = At(pick.pedals);
            p.shifterDev = At(pick.shifter);
            p.hbDev = At(pick.handbrake);
        }
        if (w.restPending && RestArrived(w.raw.axes))
        {
            w.rest = w.raw.axes;
            w.armed.assign(w.armed.size(), 0);
            w.restPending = false;
            for (int a = 0; a < w.numAxes; a++)
                Log("Wheel:   axis %d of \"%s\" rests at %d (first report)", a, w.info.name.c_str(), w.rest[a]);
        }
        const Profile* prof = FindProfile(w.info.vid, w.info.pid);
        if (prof && w.numAxes <= std::max({ kProfSteer, kProfThrottle, kProfBrake }))
            prof = nullptr; // fewer axes than the profile expects: detect instead
        const ThrustmasterModel* tm = FindThrustmaster(w.info.vid, w.info.pid);
        const PedalSources src = PlanPedals(cfg.axes, cfg.pedalAxes, p.pedals != nullptr);
        const bool fromWheel[3] = { !src.fromAux[0], !src.fromAux[1], !src.fromAux[2] };
        auto over = [&](const char* k) {
            auto it = cfg.axes.find(k);
            return it == cfg.axes.end() ? -1 : it->second;
        };
        int steer = over("steer");
        int pedal[3] = { fromWheel[0] ? over("throttle") : -1, fromWheel[1] ? over("brake") : -1, fromWheel[2] ? over("clutch") : -1 };
        if (prof)
        {
            if (steer < 0) steer = kProfSteer;
            if (pedal[0] < 0 && fromWheel[0]) pedal[0] = kProfThrottle;
            if (pedal[1] < 0 && fromWheel[1]) pedal[1] = kProfBrake;
            if (pedal[2] < 0 && fromWheel[2] && kProfClutch < w.numAxes) pedal[2] = kProfClutch;
        }
        else if (const ThrustmasterModel* t = ThrustmasterAxes(tm, w.numAxes))
        {
            if (steer < 0) steer = t->steer;
            if (pedal[0] < 0 && fromWheel[0]) pedal[0] = t->throttle;
            if (pedal[1] < 0 && fromWheel[1]) pedal[1] = t->brake;
            if (pedal[2] < 0 && fromWheel[2]) pedal[2] = t->clutch;
        }
        if (steer < 0)
        {
            for (int a = 0; a < w.numAxes; a++)
                if (std::abs(w.rest[a]) < 12000 && a != pedal[0] && a != pedal[1] && a != pedal[2])
                {
                    steer = a;
                    break;
                }
            if (steer < 0 && w.numAxes > 0)
                steer = 0;
        }
        p.combined = cfg.pedals == "combined";
        const HandbrakePlan hb = PlanHandbrake(cfg.axes, p.hbDev != nullptr, cfg.handbrakeAxis);
        const bool hbOnPedals = hb.source == HandbrakeSource::Device && p.pedals && p.hbDev == p.pedals;
        if (!prof)
            DetectPedalAxes(pedal, fromWheel, steer, hb.source == HandbrakeSource::Wheel ? RestWithout(w.rest, hb.axis) : w.rest, p.combined);
        int auxPedal[3] = { -1, -1, -1 };
        if (src.Any() && p.pedals)
            PedalDeviceAxes(auxPedal, src.fromAux, cfg.pedalAxes, FindAuxModel(p.pedals->info.vid, p.pedals->info.pid),
                            hbOnPedals ? RestWithout(p.pedals->rest, hb.axis) : p.pedals->rest, p.combined);
        p.steer = { &w, steer, false };
        for (int k = 0; k < 3; k++)
            p.pedal[k] = src.fromAux[k] ? Ax{ p.pedals, auxPedal[k], false } : Ax{ &w, pedal[k], prof != nullptr };
        if (hb.source == HandbrakeSource::Wheel)
            p.handbrake = { &w, hb.axis, false };
        else if (hb.source == HandbrakeSource::Device)
            p.handbrake = { p.hbDev, hb.axis, false };

        const std::string& mode = cfg.shifter;
        const bool auxShifter = mode != "off" && p.shifterDev;
        if (mode != "off")
            p.shifter = auxShifter ? AuxShifterMap(FindAuxModel(p.shifterDev->info.vid, p.shifterDev->info.pid), cfg.shifterDeviceButtons)
                                   : ApplyShifterOverrides(DefaultShifter(w.info.vid, w.info.pid), cfg.shifterButtons);
        p.shifterOn = auxShifter ? p.shifterDev : &w;
        p.overrides = ThrustmasterButtons(tm, cfg.buttons);
        p.buttons = WheelButtons(ButtonLayoutOf(w.info.vid, w.info.pid), p.overrides,
                                 auxShifter ? std::set<int>{} : ShifterButtons(p.shifter, mode));
        return p;
    }

    // A pedal (or handbrake) axis, 0 released .. 1 pressed. Nothing until it has been seen at rest.
    static float Pressed(const Ax& x, bool invert)
    {
        Dev* d = x.dev;
        const int a = x.axis;
        if (!d || a < 0 || a >= d->numAxes)
            return 0.0f;
        const float v = Norm(d->raw.axes[a]);
        const float r = x.known ? 1.0f : Norm(d->rest[a]);
        bool highRest = r > 0.0f;
        if (invert)
            highRest = !highRest;
        const float rest = (invert || x.known) ? (highRest ? 1.0f : -1.0f) : r;
        if (!d->armed[a])
        {
            if (std::fabs(v - rest) < 0.2f)
                d->armed[a] = 1;
            else
                return 0.0f;
        }
        const float p = highRest ? (rest - v) / (rest + 1.0f) : (v - rest) / (1.0f - rest);
        const float d0 = 0.02f;
        return std::clamp((p - d0) / (1.0f - d0), 0.0f, 1.0f);
    }

    Pad Compute(Dev& w, WheelState& st, bool primary, bool gameRead, bool extras, Readout* ro)
    {
        const Plan p = MakePlan(w, primary);
        float thr, brk;
        if (p.combined)
        {
            const Ax& t = p.pedal[0];
            const float v = t.dev && t.axis >= 0 && t.axis < t.dev->numAxes ? Norm(t.dev->raw.axes[t.axis]) : 0.0f;
            const float flip = cfg.invertThrottle ? -1.0f : 1.0f;
            thr = std::clamp(-v * flip, 0.0f, 1.0f);
            brk = std::clamp(v * flip, 0.0f, 1.0f);
        }
        else
        {
            thr = Pressed(p.pedal[0], cfg.invertThrottle);
            brk = Pressed(p.pedal[1], cfg.invertBrake);
        }
        thr = std::pow(thr, float(cfg.throttleCurve));
        brk = std::pow(brk, float(cfg.brakeCurve));
        const float clutch = cfg.clutch ? Pressed(p.pedal[2], cfg.invertClutch) : 0.0f;
        const float handbrake = Pressed(p.handbrake, cfg.invertHandbrake);

        float s = 0.0f;
        if (p.steer.axis >= 0 && p.steer.axis < w.numAxes)
            s = Norm(w.raw.axes[p.steer.axis]);
        s *= float(cfg.rotation) / float(std::max(1, cfg.lock)) * (cfg.invertSteering ? -1.0f : 1.0f);
        const float dz = float(cfg.deadzone), as = std::fabs(s);
        s = as <= dz ? 0.0f : std::copysign(std::min(1.0f, (as - dz) / (1.0f - dz)), s);
        // In the 270 degree mode the game filters the steering and has an inner deadzone: any movement
        // beyond a hair already gives `off` of the full steering. Full-range steering needs none.
        const float off = cfg.steering900 ? 0.0f : float(cfg.steerOffset);
        if (off > 0.0f && std::fabs(s) > 0.001f)
            s = std::copysign(off + (1.0f - off) * std::fabs(s), s);

        uint16_t buttons = 0;
        for (auto& [key, bit] : kPadBits)
        {
            auto it = p.buttons.find(key);
            const int n = it == p.buttons.end() ? -1 : it->second;
            if (n >= 0 && n < w.numButtons && w.raw.buttons[n])
                buttons |= bit;
        }
        if (w.numHats > 0)
            buttons |= HatDpad(w.raw.hats[0], p.overrides);

        int gear = kGearNeutral, lever = kGearNeutral;
        if (cfg.shifter != "off" && p.shifterOn)
        {
            Dev* sj = p.shifterOn;
            auto held = [&](int n) { return n >= 0 && n < sj->numButtons && sj->raw.buttons[n] != 0; };
            if (cfg.shifter == "h")
            {
                lever = LeverGear(p.shifter, held);
                st.gear = EngageGear(lever, st.gear, cfg.shifterClutch, clutch, float(cfg.clutchThreshold));
                const int before = st.request.request;
                gear = GearToReport(st.request, st.gear, NowMs(), gameRead, cfg.followSlot);
                if (gameRead && st.request.request != before && st.request.request != kGearNoRequest)
                    Log("Wheel: the lever moved: gear %d asked of the game", st.gear);
            }
            else
            {
                if (held(p.shifter.up)) buttons |= 0x0200;
                if (held(p.shifter.down)) buttons |= 0x0100;
            }
        }

        Pad out;
        out.buttons = buttons;
        out.lt = uint8_t(std::lround(brk * 255.0f));
        out.rt = uint8_t(std::lround(thr * 255.0f));
        out.lx = int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        // Only for a game that took this for a wheel: on a gamepad the right stick looks around.
        if (extras)
        {
            out.rx = int16_t(gear);
            out.ry = int16_t(std::lround(clutch * 32767.0f));
            out.ly = HandbrakeThumb(handbrake);
        }
        if (ro)
            *ro = { s, thr, brk, clutch, handbrake, lever, gear };
        return out;
    }

    // ---- force feedback (under the lock)

    FfState ff;
    bool ffLogged = false;
    RumbleAverage rumble;
    double rumbleLast = 0;
    bool focusCheck = false; // the game: forces fade out while its window isn't in front (FM2 playback)

    // The FM2 playback (wheel_ffb_mode = fm2), as Forza Motorsport 2 ReGPU's input/wheel.cpp does it: a
    // thread plays every block every 2 ms; the steering force eases to each new value of the game's over
    // about 10 ms (for direct drive wheels, which show every step); forces fade in over 0.5 s and out over
    // 0.15 s while the window isn't in front or the game sends no steering force for 0.25 s; FM2's own
    // strengths. FM2 has no rumble and no minimum force; here both are their own settings (wheel_rumble,
    // wheel_ffb_min_strength) and apply to either playback. (FM2 also fades out when its game says the player isn't
    // driving, from a hook on FM2's code that has no FM4 counterpart traced yet.)
    static constexpr float kFm2SmoothSeconds = 0.010f, kFm2FadeIn = 0.5f, kFm2FadeOut = 0.15f;
    static constexpr double kFm2StaleMs = 250.0;
    bool fm2Thread = false, ffInUse = false;
    float fm2Fade = 0, fm2Steering = 0, fm2Dt = 0;
    double fm2Updated = 0; // the game's last steering force

    bool Fm2() const { return cfg.ffbMode == "fm2"; }

    void StartFm2Thread()
    {
        if (fm2Thread)
            return;
        fm2Thread = true;
        Log("Wheel: force feedback plays as FM2 ReGPU does (wheel_ffb_mode = fm2): eased steering force, %d%% strength, spring and damper %d%% of that, minimum force %d%%, rumble %s",
            cfg.fm2Strength, cfg.fm2Spring, cfg.ffbMinStrength, cfg.rumble ? cfg.rumbleStyle.c_str() : "off");
        std::thread([this] {
            timeBeginPeriod(1);
            auto last = std::chrono::steady_clock::now();
            for (;;)
            {
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::chrono::duration<float>(now - last).count();
                last = now;
                {
                    std::lock_guard l(mutex);
                    if (Fm2() && ffInUse)
                    {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
                        const bool live = (!focusCheck || pid == GetCurrentProcessId()) && NowMs() - fm2Updated < kFm2StaleMs;
                        fm2Fade = live ? std::min(1.0f, fm2Fade + dt / kFm2FadeIn) : std::max(0.0f, fm2Fade - dt / kFm2FadeOut);
                        fm2Dt = dt;
                        for (Dev* d : FfWheels())
                            for (uint32_t b = 1; b <= kFfMaxBlocks; b++)
                                ApplyBlock(*d, b);
                    }
                }
                std::this_thread::sleep_until(now + std::chrono::milliseconds(2));
            }
        }).detach();
    }

    FfStrengths Strengths() const
    {
        // The rumble plays in either playback: under the strength of the one in use.
        FfStrengths s;
        s.overall = (Fm2() ? cfg.fm2Strength : cfg.ffbStrength) / 100.0f;
        s.spring = cfg.ffbSpringStrength / 100.0f;
        s.engine = cfg.rumbleEngineStrength / 100.0f;
        s.road = cfg.rumbleRoadStrength / 100.0f;
        return s;
    }

    // The wheels that play the game's force feedback: every wheel with force feedback.
    std::vector<Dev*> FfWheels()
    {
        std::vector<Dev*> out;
        if (!cfg.ffb)
            return out;
        for (int i : order)
            if (devs[i]->ffb)
                out.push_back(devs[i].get());
        return out;
    }

    static BOOL CALLBACK ActuatorCb(const DIDEVICEOBJECTINSTANCEW* o, void* ctx)
    {
        *static_cast<DWORD*>(ctx) = o->dwOfs;
        return DIENUM_STOP;
    }

    Haptic* OpenHaptic(Dev& d)
    {
        if (d.haptic)
            return d.haptic->failed ? nullptr : d.haptic.get();
        d.haptic = std::make_unique<Haptic>();
        Haptic& h = *d.haptic;
        IDirectInputDevice8W* dev = nullptr;
        HRESULT hr = di ? di->CreateDevice(d.instance, &dev, nullptr) : E_FAIL;
        if (SUCCEEDED(hr)) hr = dev->SetDataFormat(&c_dfDIJoystick2);
        if (SUCCEEDED(hr)) hr = dev->SetCooperativeLevel(hwnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
        if (FAILED(hr))
        {
            Log("Wheel: no force feedback on \"%s\": it could not be opened for force feedback (0x%08lX)", d.info.name.c_str(), hr);
            if (dev) dev->Release();
            h.failed = true;
            return nullptr;
        }
        // The game's spring centres the wheel from now on.
        DIPROPDWORD ac{};
        ac.diph.dwSize = sizeof(ac);
        ac.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        ac.diph.dwHow = DIPH_DEVICE;
        ac.dwData = DIPROPAUTOCENTER_OFF;
        dev->SetProperty(DIPROP_AUTOCENTER, &ac.diph);
        dev->EnumObjects(ActuatorCb, &h.axis, DIDFT_AXIS | DIDFT_FFACTUATOR);
        hr = dev->Acquire();
        if (FAILED(hr))
            Log("Wheel: \"%s\" would not be acquired for force feedback (0x%08lX): another program may hold it exclusively", d.info.name.c_str(), hr);
        dev->SendForceFeedbackCommand(DISFFC_RESET);
        dev->SendForceFeedbackCommand(DISFFC_SETACTUATORSON);
        h.dev = dev;
        if (Fm2())
            Log("Wheel: force feedback on \"%s\" (axis offset %lu, FM2 ReGPU playback: %d%% strength, spring and damper %d%% of that, %s rumble%s)",
                d.info.name.c_str(), h.axis, cfg.fm2Strength, cfg.fm2Spring, cfg.rumble ? cfg.rumbleStyle.c_str() : "no", cfg.ffbReverse ? ", reversed" : "");
        else
            Log("Wheel: force feedback on \"%s\" (axis offset %lu, %d%% strength, %s rumble%s%s)", d.info.name.c_str(), h.axis,
                cfg.ffbStrength, cfg.rumble ? cfg.rumbleStyle.c_str() : "no", cfg.ffbReverse ? ", reversed" : "",
                cfg.ffbMinStrength ? (", minimum force " + std::to_string(cfg.ffbMinStrength) + "%").c_str() : "");
        return &h;
    }

    // An effect on the force feedback axis: infinite, full gain, no envelope.
    struct EffectDesc
    {
        DWORD axes[1];
        LONG dir[1] = { 0 };
        DIEFFECT e{};
        EffectDesc(DWORD axis, void* params, DWORD size)
        {
            axes[0] = axis;
            e.dwSize = sizeof(e);
            e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
            e.dwDuration = INFINITE;
            e.dwGain = DI_FFNOMINALMAX;
            e.dwTriggerButton = DIEB_NOTRIGGER;
            e.cAxes = 1;
            e.rgdwAxes = axes;
            e.rglDirection = dir;
            e.cbTypeSpecificParams = size;
            e.lpvTypeSpecificParams = params;
        }
    };

    // An HRESULT from an effect call: a device that lost its acquisition (DirectInput stops its effects
    // then) is acquired again and every effect is played again.
    bool Lost(Dev& d, HRESULT hr)
    {
        if (hr != DIERR_INPUTLOST && hr != DIERR_NOTACQUIRED && hr != DIERR_NOTEXCLUSIVEACQUIRED)
            return false;
        Haptic& h = *d.haptic;
        if (FAILED(h.dev->Acquire()))
            return true;
        Log("Wheel: force feedback on \"%s\" acquired again: its effects play again", d.info.name.c_str());
        for (uint32_t i = 0; i < kFfMaxBlocks; i++)
        {
            h.running[i] = false;
            h.sent[i].clear();
        }
        for (int k = 0; k < 3; k++)
            h.sineRunning[k] = false, h.sineMagnitude[k] = DWORD(-1);
        for (uint32_t b = 1; b <= kFfMaxBlocks; b++)
            ApplyBlock(d, b, false);
        return true;
    }

    // One of the game's effect blocks on one wheel: created, updated, started or stopped to match `ff`.
    void ApplyBlock(Dev& d, uint32_t block, bool retry = true)
    {
        Haptic* h = OpenHaptic(d);
        if (!h || !h->dev || block < 1 || block > kFfMaxBlocks)
            return;
        const size_t i = block - 1;
        DICONSTANTFORCE cf{};
        DICONDITION cond{};
        GUID guid = GUID_NULL;
        bool play = false;
        if (ff.Defined(block))
        {
            const FfEffect& e = ff.Effect(block);
            const bool centring = e.type == kFfSpring || e.type == kFfDamper;
            const float scale = Fm2() ? FfScale(e, ff.DeviceGain(), cfg.fm2Strength / 100.0f * (centring ? cfg.fm2Spring / 100.0f : 1.0f)) * fm2Fade
                                      : FfScale(e, ff.DeviceGain(), FfTypeStrength(e.type, Strengths()));
            if (e.type == kFfConstant)
            {
                // The game's force turns the wheel right when positive; on one DirectInput axis a
                // positive magnitude pulls left (SDL's steering axis, which FH1's sign was tested on).
                float level;
                if (Fm2())
                {
                    // FM2: eased towards the game's value (a step every frame is felt on a direct drive wheel),
                    // with the minimum force (wheel_ffb_min_strength) applied first, as in this port's playback.
                    const float target = ff.Active(block)
                                             ? FfMinForceLevel(e, ff.DeviceGain(), FfConstantLevel(e, scale), cfg.ffbMinStrength / 100.0f)
                                             : 0.0f;
                    fm2Steering += (target - fm2Steering) * std::min(1.0f, fm2Dt / kFm2SmoothSeconds);
                    level = fm2Steering;
                }
                else
                    level = FfMinForceLevel(e, ff.DeviceGain(), FfConstantLevel(e, scale), cfg.ffbMinStrength / 100.0f);
                cf.lMagnitude = LONG(std::lround((cfg.ffbReverse ? 1.0f : -1.0f) * level * 10000.0f));
                // FM2: not resent for the last few units of easing (32 of SDL's 32767).
                if (Fm2() && h->fx[i] && cf.lMagnitude && std::abs(cf.lMagnitude - h->sentMagnitude[i]) < 10)
                    cf.lMagnitude = h->sentMagnitude[i];
                h->sentMagnitude[i] = cf.lMagnitude;
                guid = GUID_ConstantForce;
                play = ff.Active(block);
            }
            else if (FfIsCondition(e.type))
            {
                const FfCondition c = FfConditionOf(e, scale);
                cond.lOffset = LONG(std::lround(c.center * 10000.0f));
                cond.lPositiveCoefficient = LONG(std::lround(c.rightCoeff * 10000.0f));
                cond.lNegativeCoefficient = LONG(std::lround(c.leftCoeff * 10000.0f));
                cond.dwPositiveSaturation = DWORD(std::lround(c.rightSat * 10000.0f));
                cond.dwNegativeSaturation = DWORD(std::lround(c.leftSat * 10000.0f));
                cond.lDeadBand = LONG(std::lround(c.deadband * 10000.0f));
                guid = e.type == kFfSpring ? GUID_Spring : e.type == kFfDamper ? GUID_Damper : e.type == kFfInertia ? GUID_Inertia : GUID_Friction;
                play = ff.Active(block) && FfConditionFelt(c);
            }
        }
        IDirectInputEffect*& fx = h->fx[i];
        if (!play)
        {
            if (fx && h->running[i])
            {
                fx->Stop();
                h->running[i] = false;
            }
            return;
        }
        const bool constant = IsEqualGUID(guid, GUID_ConstantForce);
        EffectDesc desc(h->axis, constant ? (void*)&cf : (void*)&cond, constant ? sizeof(cf) : sizeof(cond));
        const std::string params(reinterpret_cast<const char*>(desc.e.lpvTypeSpecificParams), desc.e.cbTypeSpecificParams);
        if (fx && !IsEqualGUID(h->fxGuid[i], guid))
        {
            fx->Stop();
            fx->Unload();
            fx->Release();
            fx = nullptr;
            h->running[i] = false;
        }
        HRESULT hr = S_OK;
        if (!fx)
        {
            hr = h->dev->CreateEffect(guid, &desc.e, &fx, nullptr);
            if (FAILED(hr))
            {
                fx = nullptr;
                if (retry && Lost(d, hr))
                    return;
                Log("Wheel: \"%s\" rejected the game's effect block %u (type %u): 0x%08lX", d.info.name.c_str(), block, ff.Effect(block).type, hr);
                return;
            }
            h->fxGuid[i] = guid;
            h->sent[i] = params;
        }
        else if (params != h->sent[i])
        {
            hr = fx->SetParameters(&desc.e, DIEP_TYPESPECIFICPARAMS | (h->running[i] ? DIEP_NORESTART : 0));
            if (FAILED(hr))
            {
                if (retry && Lost(d, hr))
                    return;
            }
            else
                h->sent[i] = params;
        }
        if (!h->running[i])
        {
            hr = fx->Start(1, 0);
            if (FAILED(hr) && retry && Lost(d, hr))
                return;
            h->running[i] = SUCCEEDED(hr);
        }
    }

    // The game's rumble as one or two sines (wheel_ff.h).
    void PlaySine(Dev& d, int k, uint32_t periodMs, float level)
    {
        Haptic& h = *d.haptic;
        DIPERIODIC p{};
        p.dwMagnitude = DWORD(std::lround(std::clamp(level, 0.0f, 1.0f) * 10000.0f));
        p.dwPeriod = periodMs * 1000;
        IDirectInputEffect*& fx = h.sine[k];
        if (!fx)
        {
            if (h.sineFailed || p.dwMagnitude == 0)
                return;
            EffectDesc desc(h.axis, &p, sizeof(p));
            const HRESULT hr = h.dev->CreateEffect(GUID_Sine, &desc.e, &fx, nullptr);
            if (FAILED(hr))
            {
                fx = nullptr;
                if (Lost(d, hr))
                    return;
                h.sineFailed = true;
                Log("Wheel: \"%s\" has no room for a vibration (0x%08lX): the game's rumble is not played", d.info.name.c_str(), hr);
                return;
            }
            h.sineMagnitude[k] = p.dwMagnitude;
        }
        else if (p.dwMagnitude != h.sineMagnitude[k])
        {
            EffectDesc desc(h.axis, &p, sizeof(p));
            const HRESULT hr = fx->SetParameters(&desc.e, DIEP_TYPESPECIFICPARAMS | (h.sineRunning[k] ? DIEP_NORESTART : 0));
            if (FAILED(hr) && Lost(d, hr))
                return;
            h.sineMagnitude[k] = p.dwMagnitude;
        }
        if (p.dwMagnitude > 0 && !h.sineRunning[k])
            h.sineRunning[k] = SUCCEEDED(fx->Start(1, 0));
        else if (p.dwMagnitude == 0 && h.sineRunning[k])
        {
            fx->Stop();
            h.sineRunning[k] = false;
        }
    }

    void PlayRumble(Dev& d, float left, float right)
    {
        if (!OpenHaptic(d) || !d.haptic->dev)
            return;
        if (ParseRumbleStyle(cfg.rumbleStyle) == RumbleStyle::Fine)
        {
            const FineRumble f = FineRumbleOf(left, right, Strengths());
            PlaySine(d, 0, f.road.periodMs, f.road.level);
            PlaySine(d, 1, f.engine.periodMs, f.engine.level);
        }
        else
        {
            const RumbleWave w = ClassicRumble(left, right, Strengths());
            PlaySine(d, 2, w.periodMs, w.level);
        }
    }

    int SourceIndex(const Dev* d)
    {
        for (size_t i = 0; i < devs.size(); i++)
            if (devs[i].get() == d)
                return int(i);
        return -1;
    }
};

System& System::Get()
{
    static System s;
    return s;
}

System::System() : impl_(std::make_unique<Impl>()) {}

void System::Start(const Config& cfg)
{
    Impl& m = *impl_;
    {
        std::lock_guard l(m.mutex);
        m.cfg = cfg;
        if (m.started)
            return;
        m.started = true;
    }
    if (cfg.mode == "off")
        Log("Wheel: wheel_mode = off: racing wheels are not read");
    std::thread([&m] { m.Thread(); }).detach();
    std::unique_lock l(m.startMutex);
    m.startCv.wait_for(l, std::chrono::seconds(5), [&] { return m.firstScan; });
}

void System::SetConfig(const Config& cfg)
{
    std::lock_guard l(impl_->mutex);
    impl_->cfg = cfg;
    impl_->Recompute();
}

Config System::GetConfig()
{
    std::lock_guard l(impl_->mutex);
    return impl_->cfg;
}

int System::Wheels()
{
    std::lock_guard l(impl_->mutex);
    return int(impl_->order.size());
}

bool System::Read(int slot, Pad& out, bool gameRead, bool focused, Readout* readout)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    if (slot < 0 || slot >= int(m.order.size()))
        return false;
    Dev& w = *m.devs[m.order[slot]];
    WheelState& st = m.states[w.serial];
    out = Pad{};
    if (!focused)
    {
        // Out of focus: nothing pressed, and an H-pattern on lever moves asks for no gear (not neutral).
        if (st.titleCaps)
            out.rx = int16_t(m.cfg.shifter == "h" && !m.cfg.followSlot ? kGearNoRequest : kGearNeutral);
        return true;
    }
    m.Pump();
    out = m.Compute(w, st, slot == 0, gameRead, st.titleCaps || !gameRead, readout);
    return true;
}

bool System::Caps(int slot, int& topGear)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    if (slot < 0 || slot >= int(m.order.size()))
        return false;
    Dev& w = *m.devs[m.order[slot]];
    WheelState& st = m.states[w.serial];
    const Impl::Plan p = m.MakePlan(w, slot == 0);
    topGear = m.cfg.shifter == "h" ? TopGear(p.shifter) : 0;
    if (!st.titleCaps)
    {
        st.titleCaps = true;
        Log("Wheel: the game asked for the capabilities of \"%s\": an Xbox 360 wheel, %s steering%s", w.info.name.c_str(),
            m.cfg.steering900 ? "900 degree (full-range)" : "270 degree", topGear ? ", with an H-pattern shifter" : "");
        if (m.cfg.shifter == "h")
        {
            const ShifterMap& s = p.shifter;
            if (!topGear)
                Log("Wheel: wheel_shifter = h, but no gear buttons are known for the %s: bind them in the launcher",
                    p.shifterDev ? "shifter device" : "wheel");
            else
                Log("Wheel: H-pattern shifter on \"%s\", buttons r=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d 7=%d", p.shifterOn->info.name.c_str(),
                    s.gear[0], s.gear[1], s.gear[2], s.gear[3], s.gear[4], s.gear[5], s.gear[6], s.gear[7]);
        }
    }
    return true;
}

bool System::HasForceFeedback(int slot)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    return m.cfg.ffb && slot >= 0 && slot < int(m.order.size()) && m.devs[m.order[slot]]->ffb;
}

bool System::FfAvailable()
{
    std::lock_guard l(impl_->mutex);
    return !impl_->FfWheels().empty();
}

bool System::FfSetEffect(const FfEffect& effect)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    const auto wheels = m.FfWheels();
    if (wheels.empty() || !m.ff.Define(effect))
        return false;
    if (!m.ffLogged)
    {
        m.ffLogged = true;
        Log("Wheel: the game drives the force feedback (XInputdFF)");
    }
    m.ffInUse = true;
    if (effect.type == kFfConstant)
        m.fm2Updated = NowMs();
    if (m.Fm2())
        return m.StartFm2Thread(), true; // the FM2 playback plays every block every 2 ms
    for (Dev* d : wheels)
        m.ApplyBlock(*d, effect.block);
    return true;
}

void System::SetFocusCheck(bool on)
{
    std::lock_guard l(impl_->mutex);
    impl_->focusCheck = on;
}

bool System::FfOperate(uint32_t block, uint32_t op)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    const auto wheels = m.FfWheels();
    if (wheels.empty() || !m.ff.Operate(block, op))
        return false;
    if (m.Fm2())
        return true;
    for (Dev* d : wheels)
        m.ApplyBlock(*d, block);
    return true;
}

bool System::FfControl(uint32_t command)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    const auto wheels = m.FfWheels();
    if (wheels.empty())
        return false;
    m.ff.Control(command);
    if (m.Fm2())
        return true;
    for (Dev* d : wheels)
        for (uint32_t b = 1; b <= kFfMaxBlocks; b++)
            m.ApplyBlock(*d, b);
    return true;
}

bool System::FfSetGain(uint32_t gain)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    const auto wheels = m.FfWheels();
    if (wheels.empty())
        return false;
    m.ff.SetDeviceGain(gain);
    if (m.Fm2())
        return true;
    for (Dev* d : wheels)
        for (uint32_t b = 1; b <= kFfMaxBlocks; b++)
            m.ApplyBlock(*d, b);
    return true;
}

bool System::FfRumble(uint16_t leftMotor, uint16_t rightMotor)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    const auto wheels = m.FfWheels();
    if (wheels.empty())
        return false;
    if (!m.cfg.rumble)
    {
        // The rumble is switched off (wheel_rumble = 0): whatever plays stops.
        for (Dev* d : wheels)
            if (d->haptic && d->haptic->dev)
                for (int k = 0; k < 3; k++)
                    m.PlaySine(*d, k, k == 1 ? kFineEnginePeriodMs : k == 0 ? kFineRoadPeriodMs : kClassicPeriodMs, 0.0f);
        return true;
    }
    m.rumble.Add(leftMotor / 65535.0f, rightMotor / 65535.0f);
    const double now = NowMs();
    // About 30 updates a second, each the average since the last; a stop goes out at once.
    if (now - m.rumbleLast < kRumbleUpdateMs && (leftMotor || rightMotor))
        return true;
    m.rumbleLast = now;
    float left, right;
    m.rumble.Take(left, right);
    if (!leftMotor && !rightMotor)
        left = right = 0.0f;
    for (Dev* d : wheels)
        m.PlayRumble(*d, left, right);
    return true;
}

void System::FfTest()
{
    // The steering force through the game's own path (block 16, a constant force at half the game's
    // range, with the player's strength and reverse setting): right for 0.7 s, then left.
    std::thread([this] {
        auto step = [&](int magnitude, bool play) {
            Impl& m = *impl_;
            std::lock_guard l(m.mutex);
            FfEffect e;
            e.block = kFfMaxBlocks;
            e.type = kFfConstant;
            e.gain = 255;
            e.magnitude = magnitude;
            m.ff.Define(e);
            m.ff.Operate(e.block, play ? 2 : kFfOpStop);
            m.ffInUse = true;
            m.fm2Updated = NowMs();
            const auto wheels = m.FfWheels();
            if (wheels.empty() && play)
                Log("Wheel: force feedback test: no wheel with force feedback (or wheel_ffb is off)");
            if (m.Fm2())
                return m.StartFm2Thread(); // plays it, with FM2's fade in
            for (Dev* d : wheels)
                m.ApplyBlock(*d, e.block);
        };
        Log("Wheel: force feedback test: right, then left (%s playback)", impl_->Fm2() ? "FM2 ReGPU" : "this port's");
        // Sent again every 50 ms, as the game does every frame (the FM2 playback fades a stale force out).
        for (int sign : { 1, -1 })
            for (int t = 0; t < 700; t += 50)
            {
                step(64 * sign, true);
                Sleep(50);
            }
        step(0, false);
        Log("Wheel: force feedback test done");
    }).detach();
}

void System::Snapshot(uint32_t& serial, std::vector<DeviceView>& devices, std::vector<RawView>& raw, PlanView& plan, Pad& pad,
                      Readout& readout)
{
    Impl& m = *impl_;
    std::lock_guard l(m.mutex);
    m.Pump();
    serial = m.listSerial;
    devices.clear();
    raw.clear();
    plan = PlanView{};
    pad = Pad{};
    readout = Readout{};
    for (size_t i = 0; i < m.devs.size(); i++)
    {
        const Dev& d = *m.devs[i];
        DeviceView v;
        v.name = d.info.name;
        v.guid = d.info.guid;
        v.vid = d.info.vid;
        v.pid = d.info.pid;
        v.buttons = d.numButtons;
        v.hats = d.numHats;
        v.axes = d.axisNames;
        v.ffb = d.ffb;
        v.driving = d.info.driving;
        v.wheelLike = m.WheelLike(d);
        v.profile = FindProfile(d.info.vid, d.info.pid) && d.numAxes > std::max({ kProfSteer, kProfThrottle, kProfBrake });
        v.rest = d.rest;
        v.role = RoleName(DetectAuxRole(d.info.vid, d.info.pid, d.info.name));
        if (const AuxModel* a = FindAuxModel(d.info.vid, d.info.pid)) v.model = a->name;
        else if (const ThrustmasterModel* t = FindThrustmaster(d.info.vid, d.info.pid)) v.model = t->name;
        else if (const Profile* p = FindProfile(d.info.vid, d.info.pid)) v.model = p->name;
        std::string use;
        auto add = [&](const std::string& u) { use += (use.empty() ? "" : "+") + u; };
        for (size_t k = 0; k < m.order.size(); k++)
            if (m.order[k] == int(i))
                add(k == 0 ? "wheel" : "wheel" + std::to_string(k + 1));
        if (!m.order.empty())
        {
            if (m.pick.pedals == int(i)) add("pedals");
            if (m.pick.shifter == int(i)) add("shifter");
            if (m.pick.handbrake == int(i)) add("handbrake");
        }
        v.use = use;
        devices.push_back(std::move(v));
        raw.push_back(d.raw);
    }
    if (m.order.empty())
        return;
    Dev& w = *m.devs[m.order[0]];
    WheelState& st = m.states[w.serial];
    const Impl::Plan p = m.MakePlan(w, true);
    auto src = [&](const Impl::Ax& x) { return Source{ x.dev && x.axis >= 0 && x.axis < x.dev->numAxes ? m.SourceIndex(x.dev) : -1, x.axis }; };
    plan.wheel = m.order[0];
    plan.pedals = m.SourceIndex(p.pedals);
    plan.shifter = m.SourceIndex(p.shifterDev);
    plan.handbrakeDev = m.SourceIndex(p.hbDev);
    plan.steer = src(p.steer);
    for (int k = 0; k < 3; k++)
        plan.pedal[k] = src(p.pedal[k]);
    plan.handbrake = src(p.handbrake);
    plan.combined = p.combined;
    for (auto& [k, v] : p.buttons)
        plan.buttons.push_back({ k, Source{ v >= 0 && v < w.numButtons ? plan.wheel : -1, v } });
    const int shifterDev = m.SourceIndex(p.shifterOn);
    const char* names[] = { "r", "1", "2", "3", "4", "5", "6", "7" };
    for (int g = 0; g <= kMaxGear; g++)
        plan.gears.push_back({ names[g], Source{ p.shifter.gear[g] >= 0 ? shifterDev : -1, p.shifter.gear[g] } });
    plan.gears.push_back({ "up", Source{ p.shifter.up >= 0 ? shifterDev : -1, p.shifter.up } });
    plan.gears.push_back({ "down", Source{ p.shifter.down >= 0 ? shifterDev : -1, p.shifter.down } });
    // The probe is no game read: it shows what the game would get once it has read the capabilities.
    pad = m.Compute(w, st, true, false, true, &readout);
}

} // namespace wheel
