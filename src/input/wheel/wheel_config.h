// The racing wheel settings: the wheel_* keys of settings.ini (the launcher's Wheel stuff tab
// writes them; docs/WHEELS.md lists them).
#pragma once

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>

#include "input/wheel/wheel_logic.h"

namespace wheel
{

struct Config
{
    std::string mode = "auto";     // auto, on (every non-gamepad joystick), off
    std::string device;            // wheel_device: the first player's wheel ("" = found)
    int rotation = 900, lock = 900; // the wheel's rotation, and the travel that gives full steering
    double deadzone = 0.0;
    double steerOffset = 0.05;     // only in the 270 degree mode
    bool invertSteering = false;
    bool steering900 = true;       // full-range (900 degree) steering; false: the 360 wheel's 270 degree mode
    std::string pedals = "auto";   // auto/separate, combined
    bool invertThrottle = false, invertBrake = false, invertClutch = false, invertHandbrake = false;
    double throttleCurve = 1.0, brakeCurve = 1.0;
    std::map<std::string, int> axes, buttons, pedalAxes, shifterButtons, shifterDeviceButtons;
    bool clutch = true;
    std::string shifter = "off";   // off, h, sequential
    bool shifterClutch = false;
    double clutchThreshold = 0.5;
    bool followSlot = true;
    std::string pedalDevice, shifterDevice, handbrakeDevice;
    int handbrakeAxis = 0;
    // Force feedback: the game's own wheel effects (wheel_ff.h), percentages of the wheel's full force.
    bool ffb = true;
    int ffbStrength = 60;         // everything
    int ffbSpringStrength = 100;  // the centring spring and the damper, of ffbStrength
    int ffbMinStrength = 0;       // minimum steering force (0: off), in either playback (ffbMode)
    int rumbleEngineStrength = 50, rumbleRoadStrength = 80; // the game's right and left rumble motors
    std::string rumbleStyle = "fine"; // fine, classic
    bool rumble = true;           // the game's rumble on the wheel, in either playback (wheel_ffb_mode)
    bool ffbReverse = false;      // reverse the steering force
    // Which playback of the game's effects (for comparing): "port" (this port's, from FH1) or "fm2"
    // (Forza Motorsport 2 ReGPU's input/wheel.cpp: the steering force eased at 500 Hz, fades, its own
    // strengths below). The rumble is separate (rumble above) and plays in either.
    std::string ffbMode = "port";
    int fm2Strength = 50, fm2Spring = 50; // FM2's wheel_strength and wheel_spring (percent)

    // From settings.ini's keys (get returns "" for a missing one).
    static Config From(const std::function<std::string(const char*)>& get)
    {
        Config c;
        auto str = [&](const char* k, std::string& out) {
            const std::string v = get(k);
            if (!v.empty())
                out = v;
        };
        auto lower = [&](const char* k, std::string& out, std::initializer_list<const char*> allowed) {
            const std::string v = AuxLower(get(k));
            for (const char* a : allowed)
                if (v == a)
                    out = v;
        };
        auto flag = [&](const char* k, bool& out) {
            const std::string v = AuxLower(get(k));
            if (v == "1" || v == "true" || v == "on" || v == "yes")
                out = true;
            else if (v == "0" || v == "false" || v == "off" || v == "no")
                out = false;
        };
        auto num = [&](const char* k, double& out, double lo, double hi) {
            const std::string v = get(k);
            if (!v.empty())
                out = std::clamp(std::atof(v.c_str()), lo, hi);
        };
        auto inum = [&](const char* k, int& out, int lo, int hi) {
            const std::string v = get(k);
            if (!v.empty())
                out = std::clamp(std::atoi(v.c_str()), lo, hi);
        };
        lower("wheel_mode", c.mode, { "auto", "on", "off" });
        str("wheel_device", c.device);
        inum("wheel_rotation", c.rotation, 90, 2520);
        inum("wheel_lock", c.lock, 90, 2520);
        num("wheel_deadzone", c.deadzone, 0.0, 0.5);
        num("wheel_steer_offset", c.steerOffset, 0.0, 0.3);
        flag("wheel_invert_steering", c.invertSteering);
        flag("wheel_900_steering", c.steering900);
        lower("wheel_pedals", c.pedals, { "auto", "separate", "combined" });
        flag("wheel_invert_throttle", c.invertThrottle);
        flag("wheel_invert_brake", c.invertBrake);
        flag("wheel_invert_clutch", c.invertClutch);
        flag("wheel_invert_handbrake", c.invertHandbrake);
        num("wheel_throttle_curve", c.throttleCurve, 0.2, 4.0);
        num("wheel_brake_curve", c.brakeCurve, 0.2, 4.0);
        c.axes = ParseKv(get("wheel_axes"));
        c.buttons = ParseKv(get("wheel_buttons"));
        c.pedalAxes = ParseKv(get("wheel_pedal_axes"));
        c.shifterButtons = ParseKv(get("wheel_shifter_buttons"));
        c.shifterDeviceButtons = ParseKv(get("wheel_shifter_device_buttons"));
        flag("wheel_clutch", c.clutch);
        lower("wheel_shifter", c.shifter, { "off", "h", "sequential" });
        flag("wheel_shifter_clutch", c.shifterClutch);
        num("wheel_clutch_threshold", c.clutchThreshold, 0.1, 1.0);
        flag("wheel_shifter_follow_slot", c.followSlot);
        str("wheel_pedal_device", c.pedalDevice);
        str("wheel_shifter_device", c.shifterDevice);
        str("wheel_handbrake_device", c.handbrakeDevice);
        inum("wheel_handbrake_axis", c.handbrakeAxis, 0, 99);
        flag("wheel_ffb", c.ffb);
        inum("wheel_ffb_strength", c.ffbStrength, 0, 100);
        inum("wheel_ffb_spring_strength", c.ffbSpringStrength, 0, 100);
        inum("wheel_ffb_min_strength", c.ffbMinStrength, 0, 100);
        inum("wheel_rumble_engine_strength", c.rumbleEngineStrength, 0, 100);
        inum("wheel_rumble_road_strength", c.rumbleRoadStrength, 0, 100);
        lower("wheel_rumble_style", c.rumbleStyle, { "fine", "classic" });
        flag("wheel_rumble", c.rumble);
        flag("wheel_ffb_reverse", c.ffbReverse);
        lower("wheel_ffb_mode", c.ffbMode, { "port", "fm2" });
        inum("wheel_fm2_strength", c.fm2Strength, 0, 100);
        inum("wheel_fm2_spring", c.fm2Spring, 0, 100);
        return c;
    }
};

} // namespace wheel
