// Racing wheels, pedals, shifters and handbrakes through DirectInput, presented to the game as
// Xbox 360 Wireless Racing Wheels (wheel_logic.h). Every wheel is a controller of its own, the
// first one (the wheel) the first player's; pedals, a shifter and a handbrake on their own USB
// devices are read together with it. The launcher shows the same devices with the same numbers
// through fm4.exe --wheel-probe (wheel_probe.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "input/wheel/wheel_config.h"

namespace wheel
{

// Where log lines go (the game's Log, the probe's stderr). printf-style.
using LogFn = void (*)(const char* line);
void SetLog(LogFn fn);

// An Xbox 360 pad state (little-endian host values).
struct Pad
{
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// What the controls read, 0..1 (steer -1..1), for the launcher's live view.
struct Readout
{
    float steer = 0, throttle = 0, brake = 0, clutch = 0, handbrake = 0;
    int lever = 0, gear = 0; // the lever's slot, and what is sent to the game (10: no request)
};

// One joystick as the launcher sees it.
struct DeviceView
{
    std::string name, guid;
    uint16_t vid = 0, pid = 0;
    int buttons = 0, hats = 0;
    std::vector<std::string> axes; // their names (X, Y, Z, Rx, Ry, Rz, Slider 1, Slider 2)
    bool ffb = false, driving = false, wheelLike = false;
    bool profile = false; // a known Logitech wheel: its pedals rest at the maximum
    std::vector<int16_t> rest; // where each axis sat when it was opened
    std::string role;  // detected: "pedals", "shifter", "handbrake" or ""
    std::string model; // a known model's name
    std::string use;   // what it is used as now: "wheel", "wheel2".., "pedals", "shifter", "handbrake" (a "+" list), ""
};
struct RawView
{
    std::vector<int16_t> axes;
    std::vector<uint8_t> buttons;
    std::vector<uint8_t> hats; // up 1, right 2, down 4, left 8
};
// Where each control of the first wheel reads (device index into the device list, -1: none).
struct Source
{
    int dev = -1, index = -1;
};
struct PlanView
{
    int wheel = -1, pedals = -1, shifter = -1, handbrakeDev = -1;
    Source steer, pedal[3], handbrake;
    bool combined = false;
    std::vector<std::pair<std::string, Source>> buttons; // pad button -> wheel button
    std::vector<std::pair<std::string, Source>> gears;   // "r", "1".."7", "up", "down"
};

class System
{
public:
    static System& Get();
    // Opens DirectInput and scans for devices (the first scan before returning). Idempotent.
    void Start(const Config& cfg);
    void SetConfig(const Config& cfg);
    Config GetConfig();
    // The wheels presented to the game (guest users 0..Wheels()-1).
    int Wheels();
    // The state of wheel `slot`; false when there is none. gameRead: the game reads it (not a probe);
    // focused: the game window is in front (otherwise nothing is pressed).
    bool Read(int slot, Pad& out, bool gameRead, bool focused, Readout* readout = nullptr);
    // The capabilities the game reads: from then on it takes the wheel for a wheel, so the clutch, the
    // gear and the handbrake go to it. False when there is no such wheel.
    bool Caps(int slot, int& topGear);
    // Whether wheel `slot` can play force feedback (and wheel_ffb is on): its capabilities say so.
    bool HasForceFeedback(int slot);
    // The game's force feedback, from the kernel's XInputdFF* calls (src/kernel/imports_ff.cpp,
    // wheel_ff.h), played on every wheel with force feedback. Each is false when there is none.
    bool FfAvailable();
    bool FfSetEffect(const struct FfEffect& effect);
    bool FfOperate(uint32_t block, uint32_t op);
    bool FfControl(uint32_t command);
    bool FfSetGain(uint32_t gain);
    bool FfRumble(uint16_t leftMotor, uint16_t rightMotor);
    // The launcher's test (wheel_probe.cpp): the steering force to the right, then to the left.
    void FfTest();
    // The game: the FM2 playback (wheel_ffb_mode = fm2) fades the forces out while its window isn't in
    // front, as FM2 does. Off for the launcher's probe.
    void SetFocusCheck(bool on);
    // For the launcher (wheel_probe.cpp). `serial` changes whenever the device list does.
    void Snapshot(uint32_t& serial, std::vector<DeviceView>& devices, std::vector<RawView>& raw, PlanView& plan, Pad& pad,
                  Readout& readout);

    struct Impl;

private:
    System();
    std::unique_ptr<Impl> impl_;
};

// fm4.exe --wheel-probe: device list and live state as JSON lines for the launcher.
int ProbeMain();

} // namespace wheel
