// Racing wheel logic without any device API: which joystick is the wheel, the pedals, the shifter
// and the handbrake, which axis and button each control reads, and how an H-pattern shifter's gear
// reaches the game. wheel_system.cpp feeds it what DirectInput reports.
//
// Ported from Forza Horizon 1: ReImagined's racing wheel driver (its ReXGlue SDK patch series
// 0074, 0099, 0101, 0127, 0128, 0132, 0133, 0134 and 0135: wheel_shifter.h, wheel_buttons.h,
// wheel_devices.h, wheel_gear_request.h, wheel_handbrake.h, wheel_primary.h and
// wheel_thrustmaster.h), https://github.com/FH1-ReImagined/ForzaHorizon1-ReImagined, whose files
// carry the BSD 3-Clause License (see NOTICE-wheel.md). Their SDL joystick numbers are kept as the
// numbering here: axes in HID usage order (X, Y, Z, Rx, Ry, Rz, sliders), buttons from 0.
//
// The Xbox 360 Wireless Racing Wheel as the Forza engine reads it (traced in FH1, which is built on
// the same engine as Forza Motorsport 4; see docs/WHEELS.md for what is and isn't verified in FM4):
//  - capabilities: SubType 2 (XINPUT_DEVSUBTYPE_WHEEL); thumb_lx 900 asks for the game's
//    full-range (900 degree) steering, anything else is the 360 wheel's 270 degree mode; thumb_rx
//    other than 0 and 1 says there is an H-pattern shifter, its high byte the forward gears;
//  - state: steering on the left stick X, throttle and brake on the right and left trigger, the
//    clutch on the right stick Y (0 released, 32767 pressed), the H-pattern's gear on the right stick
//    X (-1 reverse, 0 neutral, 1 to 8; 10 asks for nothing), and an analog handbrake on the left
//    stick Y (pulled = up). The bumpers are the paddles (RB up, LB down).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wheel
{

// "a=0,b=1" to name -> number (keys lower case, spaces dropped).
inline std::map<std::string, int> ParseKv(const std::string& s)
{
    std::map<std::string, int> out;
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t end = s.find(',', pos);
        if (end == std::string::npos)
            end = s.size();
        const std::string item = s.substr(pos, end - pos);
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
        {
            std::string key;
            for (unsigned char c : item.substr(0, eq))
                if (!std::isspace(c))
                    key += char(std::tolower(c));
            if (!key.empty())
                out[key] = std::atoi(item.c_str() + eq + 1);
        }
        pos = end + 1;
    }
    return out;
}

inline float Norm(int16_t v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

// Wheels -----------------------------------------------------------------------------------------

inline bool ContainsCI(std::string_view h, std::string_view needle)
{
    return std::search(h.begin(), h.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           }) != h.end();
}

inline bool NameLooksLikeWheel(std::string_view name)
{
    static constexpr std::string_view kWords[] = { "wheel", "racing", "driving force", "g25", "g27", "g29", "g920", "g923",
        "t300", "t150", "t248", "t-gt", "tx racing", "fanatec", "moza", "simagic", "csl", "podium", "ts-xw" };
    for (auto w : kWords)
        if (ContainsCI(name, w))
            return true;
    return false;
}

// Known Logitech wheels by USB vendor:product: fixed axes, pedals that rest at the maximum and read
// the minimum when pressed, so nothing is guessed from where the axes sit when the wheel is opened.
// Axis numbers on Windows: steering 0, throttle 1, brake 2, clutch 3 (X, Y, Rz, slider).
struct Profile
{
    uint16_t vid, pid;
    const char* name;
};
inline constexpr Profile kProfiles[] = {
    { 0x046D, 0xC24F, "Logitech G29" },
    { 0x046D, 0xC262, "Logitech G920" },
    { 0x046D, 0xC266, "Logitech G923" },
    { 0x046D, 0xC26E, "Logitech G923 (Xbox)" },
    { 0x046D, 0xC29A, "Logitech Driving Force GT" },
    { 0x046D, 0xC29B, "Logitech G27" },
    { 0x046D, 0xC299, "Logitech G25" },
};
constexpr int kProfSteer = 0, kProfThrottle = 1, kProfBrake = 2, kProfClutch = 3;

inline const Profile* FindProfile(uint16_t vid, uint16_t pid)
{
    for (const auto& p : kProfiles)
        if (p.vid == vid && p.pid == pid)
            return &p;
    return nullptr;
}

// Thrustmaster wheels by product id (Project Motor Racing's device maps): steering X, brake Y,
// throttle Rz, clutch slider, the paddles on the bumpers. The numbers hold only with the axis count
// the map assumes.
struct ThrustmasterModel
{
    uint16_t pid;
    const char* name;
    int axes;
    int steer, throttle, brake, clutch;
    int a, b, x, y, lb, rb, back, start, ls, rs;
};
constexpr uint16_t kThrustmasterVid = 0x044F;
inline constexpr ThrustmasterModel kThrustmasterModels[] = {
    { 0xB67F, "Thrustmaster TMX", 4, 0, 2, 1, 3, 4, 8, 2, 9, 0, 1, 6, 7, 5, 10 },
    { 0xB669, "Thrustmaster TX", 4, 0, 2, 1, 3, 4, 8, 2, 9, 0, 1, 3, 5, -1, -1 },
    { 0xB692, "Thrustmaster TS-XW", 4, 0, 2, 1, 3, 12, 8, 2, 9, 0, 1, 6, 7, -1, -1 },
    { 0xB689, "Thrustmaster TS-PC Racer", 4, 0, 2, 1, 3, 12, 8, 2, 9, 0, 1, 6, 7, -1, -1 },
    { 0xB66E, "Thrustmaster T300 RS", 4, 0, 2, 1, 3, 5, 4, 3, 2, 0, 1, 6, 7, 10, 11 },
    { 0xB677, "Thrustmaster T150", 4, 0, 2, 1, 3, 5, 4, 3, 2, 0, 1, 6, 7, 10, 11 },
    { 0xB684, "Thrustmaster T-GT", 8, 0, 5, 1, 6, 5, 4, 3, 2, 0, 1, 6, 7, 10, 11 },
};

inline const ThrustmasterModel* FindThrustmaster(uint16_t vid, uint16_t pid)
{
    if (vid != kThrustmasterVid)
        return nullptr;
    for (const auto& m : kThrustmasterModels)
        if (m.pid == pid)
            return &m;
    return nullptr;
}
inline const ThrustmasterModel* ThrustmasterAxes(const ThrustmasterModel* m, int numAxes)
{
    return m && numAxes == m->axes ? m : nullptr;
}
inline std::map<std::string, int> ThrustmasterButtons(const ThrustmasterModel* m, const std::map<std::string, int>& user)
{
    if (!m)
        return user;
    std::map<std::string, int> out = { { "a", m->a }, { "b", m->b }, { "x", m->x }, { "y", m->y }, { "lb", m->lb },
        { "rb", m->rb }, { "back", m->back }, { "start", m->start }, { "ls", m->ls }, { "rs", m->rs } };
    for (const auto& [k, v] : user)
        out[k] = v;
    return out;
}

// Every axis read exactly 0 when the wheel was opened: no report yet, so where it rests is unknown
// (a pedal rests at an end). Its rest is read again once a report comes in.
inline bool RestPending(const std::vector<int16_t>& rest)
{
    if (rest.size() < 2)
        return false;
    for (int16_t v : rest)
        if (v != 0)
            return false;
    return true;
}
inline bool RestArrived(const std::vector<int16_t>& now)
{
    for (int16_t v : now)
        if (v != 0)
            return true;
    return false;
}

// Shifter ----------------------------------------------------------------------------------------

constexpr int kGearReverse = -1;
constexpr int kGearNeutral = 0;
constexpr int kMaxGear = 7;

struct ShifterMap
{
    int gear[kMaxGear + 1] = { -1, -1, -1, -1, -1, -1, -1, -1 }; // [0] reverse, [1..7] forward gears
    int up = -1, down = -1;                                       // a sequential shifter
};

// The Logitech Driving Force Shifter on a G29, G920 or G923: gears 1-6 on buttons 12-17, reverse 18
// on a G29 or PlayStation G923 and 11 on a G920 or Xbox G923.
inline ShifterMap LogitechShifter(int reverse)
{
    ShifterMap m;
    for (int g = 1; g <= 6; g++)
        m.gear[g] = 11 + g;
    m.gear[0] = reverse;
    return m;
}
inline ShifterMap DefaultShifter(uint16_t vid, uint16_t pid)
{
    if (vid != 0x046D)
        return {};
    switch (pid)
    {
    case 0xC24F: case 0xC266: case 0xC267: return LogitechShifter(18);
    case 0xC262: case 0xC26E: case 0xC26D: return LogitechShifter(11);
    default: return {};
    }
}
// "1=12,...,6=17,r=18" and "up=..,down=..".
inline ShifterMap ApplyShifterOverrides(ShifterMap m, const std::map<std::string, int>& kv)
{
    for (const auto& [key, n] : kv)
    {
        if (key == "r")
            m.gear[0] = n;
        else if (key == "up")
            m.up = n;
        else if (key == "down")
            m.down = n;
        else if (key.size() == 1 && key[0] >= '1' && key[0] <= '0' + kMaxGear)
            m.gear[key[0] - '0'] = n;
    }
    return m;
}
inline int TopGear(const ShifterMap& m)
{
    for (int g = kMaxGear; g >= 1; g--)
        if (m.gear[g] >= 0)
            return g;
    return 0;
}
// The gear the lever is in: the one held gear button, neutral when none (or more than one) is.
template <typename Held> int LeverGear(const ShifterMap& m, Held&& held)
{
    int gear = kGearNeutral, count = 0;
    for (int g = 0; g <= kMaxGear; g++)
        if (m.gear[g] >= 0 && held(m.gear[g]))
        {
            gear = g == 0 ? kGearReverse : g;
            count++;
        }
    return count == 1 ? gear : kGearNeutral;
}
// With needClutch a new gear engages only while the clutch (0..1) is at the threshold or further.
inline int EngageGear(int lever, int engaged, bool needClutch, float clutch, float threshold)
{
    if (lever == engaged)
        return engaged;
    if (lever == kGearNeutral || !needClutch || clutch >= threshold)
        return lever;
    return kGearNeutral;
}
// The buttons the shifter uses, which press no pad button.
inline std::set<int> ShifterButtons(const ShifterMap& m, const std::string& mode)
{
    std::set<int> out;
    if (mode == "h")
        for (int b : m.gear)
            if (b >= 0)
                out.insert(b);
    if (mode == "sequential")
    {
        if (m.up >= 0) out.insert(m.up);
        if (m.down >= 0) out.insert(m.down);
    }
    return out;
}

// The H-pattern gear as the game's gear request. With followSlot (the default, as on the Xbox) the
// lever's slot is asked for all the time, so race starts put the car in the lever's gear; without
// it a gear is asked for only for a moment after the lever moves, so the paddles keep their gear.
constexpr int kGearNoRequest = 10;
constexpr double kGearRequestMs = 250.0;
constexpr int kGearRequestReads = 4;
struct GearRequest
{
    bool started = false;
    int engaged = kGearNeutral;
    int request = kGearNoRequest;
    double sinceMs = 0.0;
    int reads = 0;
};
inline int GearToReport(GearRequest& r, int engaged, double nowMs, bool gameRead, bool followSlot)
{
    if (followSlot || !r.started)
    {
        r = GearRequest{};
        r.started = true;
        r.engaged = engaged;
        return followSlot ? engaged : kGearNoRequest;
    }
    if (engaged != r.engaged)
    {
        r.engaged = engaged;
        r.request = engaged;
        r.sinceMs = nowMs;
        r.reads = 0;
    }
    if (r.request == kGearNoRequest)
        return kGearNoRequest;
    const int out = r.request;
    if (gameRead && ++r.reads >= kGearRequestReads && nowMs - r.sinceMs >= kGearRequestMs)
        r.request = kGearNoRequest;
    return out;
}

// Buttons ----------------------------------------------------------------------------------------

enum class ButtonLayout
{
    Generic,     // face buttons, right and left paddle, Back, Start, the stick presses at 8-11
    PlayStation, // G29, G923 for PlayStation: cross, square, circle, triangle, paddles, R2, L2, ...
    Xbox,        // G920, G923 for Xbox: A, B, X, Y, paddles, Menu, View, RSB, LSB, Xbox, ...
};
inline ButtonLayout ButtonLayoutOf(uint16_t vid, uint16_t pid)
{
    if (vid == 0x046D)
        switch (pid)
        {
        case 0xC24F: case 0xC266: case 0xC267: return ButtonLayout::PlayStation;
        case 0xC262: case 0xC26E: case 0xC26D: return ButtonLayout::Xbox;
        default: break;
        }
    return ButtonLayout::Generic;
}
// Pad button name -> wheel button (-1: none). `overrides` is wheel_buttons; a button in `taken`
// (the shifter's) presses no pad button, whatever the defaults or the overrides say.
inline std::map<std::string, int> WheelButtons(ButtonLayout layout, const std::map<std::string, int>& overrides,
                                               const std::set<int>& taken = {})
{
    std::map<std::string, int> map = { { "a", 0 }, { "b", 1 }, { "x", 2 }, { "y", 3 }, { "rb", 4 }, { "lb", 5 },
        { "back", 8 }, { "start", 9 }, { "rs", 10 }, { "ls", 11 }, { "up", -1 }, { "down", -1 }, { "left", -1 },
        { "right", -1 } };
    if (layout == ButtonLayout::PlayStation)
    {
        map["b"] = 2;
        map["x"] = 1;
    }
    else if (layout == ButtonLayout::Xbox)
    {
        map["start"] = 6;
        map["back"] = 7;
        map["rs"] = 8;
        map["ls"] = 9;
    }
    for (const auto& [k, v] : overrides)
        if (map.count(k))
            map[k] = v;
    for (auto& [k, v] : map)
        if (taken.count(v))
            v = -1;
    return map;
}
// Hat bits: up 1, right 2, down 4, left 8 (SDL's). A d-pad direction wheel_buttons sets replaces the
// hat's (a button number presses it instead, -1 turns it off).
constexpr uint8_t kHatUp = 1, kHatRight = 2, kHatDown = 4, kHatLeft = 8;
inline uint16_t HatDpad(uint8_t hat, const std::map<std::string, int>& overrides)
{
    static constexpr struct
    {
        uint8_t hat;
        const char* key;
        uint16_t bit;
    } kDirs[] = { { kHatUp, "up", 0x0001 }, { kHatDown, "down", 0x0002 }, { kHatLeft, "left", 0x0004 }, { kHatRight, "right", 0x0008 } };
    uint16_t out = 0;
    for (const auto& d : kDirs)
        if ((hat & d.hat) && !overrides.count(d.key))
            out |= d.bit;
    return out;
}
// The 360 pad's button bits by name.
struct PadBit
{
    const char* name;
    uint16_t bit;
};
inline constexpr PadBit kPadBits[] = { { "up", 0x0001 }, { "down", 0x0002 }, { "left", 0x0004 },
    { "right", 0x0008 }, { "start", 0x0010 }, { "back", 0x0020 }, { "ls", 0x0040 }, { "rs", 0x0080 }, { "lb", 0x0100 },
    { "rb", 0x0200 }, { "a", 0x1000 }, { "b", 0x2000 }, { "x", 0x4000 }, { "y", 0x8000 } };

// Pedals, shifters and handbrakes on their own USB device --------------------------------------

enum class AuxRole { None, Pedals, Shifter, Handbrake };

struct AuxModel
{
    uint16_t vid, pid;
    AuxRole role;
    const char* name;
    int axes = 0; // pedals: the axis count the layout below assumes
    int throttle = -1, brake = -1, clutch = -1;
    int firstGear = -1; // shifter: button of gear 1; the others follow
    int gears = 0;
    int reverse = -1;
    int up = -1, down = -1;
};
// Sources (FH1's comments): simracing-hwdb, Project Motor Racing's device maps, hid-fanatecff and
// SimagicHPR.
inline constexpr AuxModel kAuxModels[] = {
    { 0x0483, 0x0525, AuxRole::Pedals, "Simagic P1000", 3, 2, 1, 0 },
    { 0x0483, 0x0524, AuxRole::Pedals, "Simagic P2000", 3, 2, 1, 0 },
    { 0x3670, 0x0902, AuxRole::Pedals, "Simagic P2000 (haptic)", 3, 2, 1, 0 },
    { 0x3670, 0x0903, AuxRole::Pedals, "Simagic P500" },
    { 0x3670, 0x0905, AuxRole::Pedals, "Simagic P700" },
    { 0x0483, 0x0531, AuxRole::Shifter, "Simagic DS-8X", 0, -1, -1, -1, 0, 7, 8, 10, 9 },
    { 0x0483, 0x0530, AuxRole::Handbrake, "Simagic TB-1 handbrake" },
    { 0x3670, 0x0A04, AuxRole::Handbrake, "Simagic TB-RS handbrake" },
    { 0x0EB7, 0x6204, AuxRole::Pedals, "Fanatec CSL Elite Pedals LC", 3, 0, 1, 2 },
    { 0x0EB7, 0x6205, AuxRole::Pedals, "Fanatec CSL Pedals" },
    { 0x0EB7, 0x6206, AuxRole::Pedals, "Fanatec CSL Elite Pedals LC V2", 3, 0, 1, 2 },
    { 0x0EB7, 0x183B, AuxRole::Pedals, "Fanatec ClubSport Pedals V3", 3, 0, 1, 2 },
    { 0x0EB7, 0x1A92, AuxRole::Shifter, "Fanatec ClubSport Shifter SQ", 0, -1, -1, -1, 1, 7, 0, 9, 8 },
    { 0x0EB7, 0x1A93, AuxRole::Handbrake, "Fanatec ClubSport Handbrake" },
    { 0x044F, 0xB371, AuxRole::Pedals, "Thrustmaster T-LCM", 3, 0, 2, 1 },
    { 0x044F, 0xB660, AuxRole::Shifter, "Thrustmaster TH8 RS", 0, -1, -1, -1, 0, 7, 7, 9, 8 },
    { 0x044F, 0xB695, AuxRole::Handbrake, "Thrustmaster TSS handbrake" },
    { 0x046D, 0xC26A, AuxRole::Pedals, "Logitech G PRO Racing Pedals", 3, 0, 1, 2 },
    { 0x046D, 0xC277, AuxRole::Pedals, "Logitech RS Pedals" },
    { 0x046D, 0xC278, AuxRole::Shifter, "Logitech RS Shifter and Handbrake" },
    { 0x046D, 0xC273, AuxRole::Shifter, "Logitech Driving Force Shifter (USB adapter)", 0, -1, -1, -1, 0, 6, 6 },
    { 0x30B7, 0x1001, AuxRole::Pedals, "Heusinkveld Sprint", 3, 0, 1, 2 },
    { 0x30B7, 0x1003, AuxRole::Pedals, "Heusinkveld Ultimate", 3, 0, 1, 2 },
    { 0x30B7, 0x1004, AuxRole::Pedals, "Heusinkveld Race Center", 3, 0, 1, 2 },
    { 0x10C4, 0x8B02, AuxRole::Pedals, "Heusinkveld Pedals (DC)", 3, 0, 1, 2 },
    { 0xA020, 0x3142, AuxRole::Shifter, "Heusinkveld Sequential Shifter" },
    { 0x30B7, 0x1005, AuxRole::Shifter, "Heusinkveld Shifter" },
    { 0x30B7, 0x1006, AuxRole::Shifter, "Heusinkveld Shifter" },
    { 0x346E, 0x0003, AuxRole::Pedals, "Moza SR-P", 3, 0, 1, 2 },
    { 0x346E, 0x0001, AuxRole::Pedals, "Moza CRP", 3, 0, 1, 2 },
    { 0x346E, 0x0008, AuxRole::Pedals, "Moza mBooster", 3, 0, 1, 2 },
    { 0x346E, 0x0013, AuxRole::Pedals, "Moza mBooster", 3, 0, 1, 2 },
    { 0x346E, 0x001E, AuxRole::Shifter, "Moza HGP", 0, -1, -1, -1, 5, 7, 4 },
    { 0x346E, 0x0023, AuxRole::Shifter, "Moza SGP" },
};

inline const AuxModel* FindAuxModel(uint16_t vid, uint16_t pid)
{
    for (const auto& m : kAuxModels)
        if (m.vid == vid && m.pid == pid)
            return &m;
    return nullptr;
}

// Lower case, runs of white space as one, trimmed.
inline std::string AuxLower(std::string_view s)
{
    std::string out;
    bool space = false;
    for (unsigned char c : s)
    {
        if (std::isspace(c))
        {
            space = !out.empty();
            continue;
        }
        if (space)
        {
            out += ' ';
            space = false;
        }
        out += char(std::tolower(c));
    }
    return out;
}

// What a device's name says it is; a name that also says "wheel" is a wheel base.
inline AuxRole AuxRoleByName(std::string_view name)
{
    const std::string n = AuxLower(name);
    auto has = [&](std::string_view w) { return n.find(w) != std::string::npos; };
    if (has("wheel"))
        return AuxRole::None;
    if (has("pedal"))
        return AuxRole::Pedals;
    if (has("shifter") || has("th8a") || has("th8 rs") || has("gear shift"))
        return AuxRole::Shifter;
    if (has("handbrake") || has("hand brake"))
        return AuxRole::Handbrake;
    return AuxRole::None;
}
inline AuxRole DetectAuxRole(uint16_t vid, uint16_t pid, std::string_view name)
{
    if (const AuxModel* m = FindAuxModel(vid, pid))
        return m->role;
    return AuxRoleByName(name);
}

// One joystick as the pickers see it.
struct JoyInfo
{
    std::string name;
    std::string guid; // DirectInput's instance GUID
    uint16_t vid = 0, pid = 0;
    bool driving = false; // the system lists it as a wheel (DI8DEVTYPE_DRIVING)
};

// A device setting: "" or "auto" finds one, "none" (also "off", "wheel") reads from the wheel,
// anything else names one: a name, the GUID or "vid:pid" in hex.
enum class DeviceChoice { Auto, None, Named };
inline DeviceChoice ParseDeviceChoice(const std::string& setting)
{
    const std::string s = AuxLower(setting);
    if (s.empty() || s == "auto")
        return DeviceChoice::Auto;
    if (s == "none" || s == "off" || s == "wheel")
        return DeviceChoice::None;
    return DeviceChoice::Named;
}
inline std::string VidPid(uint16_t vid, uint16_t pid)
{
    char b[10];
    std::snprintf(b, sizeof(b), "%04x:%04x", vid, pid);
    return b;
}
// 2: exactly (name, GUID or vid:pid), 1: the name contains the setting, 0: no.
inline int DeviceMatch(const std::string& setting, const JoyInfo& j)
{
    const std::string s = AuxLower(setting);
    if (s.empty())
        return 0;
    if (s == AuxLower(j.name) || s == AuxLower(j.guid) || (j.vid && s == VidPid(j.vid, j.pid)))
        return 2;
    return AuxLower(j.name).find(s) != std::string::npos ? 1 : 0;
}
inline int PickNamed(const std::vector<JoyInfo>& joys, const std::string& setting)
{
    int best = -1, bestScore = 0;
    for (int i = 0; i < int(joys.size()); i++)
    {
        const int m = DeviceMatch(setting, joys[i]);
        if (!m)
            continue;
        const int score = m * 2 + (joys[i].driving ? 0 : 1);
        if (score > bestScore)
            best = i, bestScore = score;
    }
    return best;
}

struct AuxPick
{
    int pedals = -1, shifter = -1, handbrake = -1;
    std::vector<int> other; // a second pedal set, an unused handbrake: no wheel either
    bool IsAux(int i) const
    {
        return i == pedals || i == shifter || i == handbrake || std::find(other.begin(), other.end(), i) != other.end();
    }
};
// The pedal set, the shifter and the handbrake among the joysticks (-1: none). A named setting takes
// its best match; auto the first known (or named-like) device of the role. One device may be two.
inline AuxPick PickAuxDevices(const std::vector<JoyInfo>& joys, const std::string& pedalSetting,
                              const std::string& shifterSetting, const std::string& handbrakeSetting)
{
    AuxPick p;
    const DeviceChoice pc = ParseDeviceChoice(pedalSetting), sc = ParseDeviceChoice(shifterSetting),
                       hc = ParseDeviceChoice(handbrakeSetting);
    if (pc == DeviceChoice::Named)
        p.pedals = PickNamed(joys, pedalSetting);
    if (sc == DeviceChoice::Named)
        p.shifter = PickNamed(joys, shifterSetting);
    if (hc == DeviceChoice::Named)
        p.handbrake = PickNamed(joys, handbrakeSetting);
    for (int i = 0; i < int(joys.size()); i++)
    {
        const AuxRole role = DetectAuxRole(joys[i].vid, joys[i].pid, joys[i].name);
        if (role == AuxRole::Pedals && pc == DeviceChoice::Auto && p.pedals < 0)
            p.pedals = i;
        else if (role == AuxRole::Shifter && sc == DeviceChoice::Auto && p.shifter < 0)
            p.shifter = i;
        else if (role == AuxRole::Handbrake && hc == DeviceChoice::Auto && p.handbrake < 0)
            p.handbrake = i;
        else if (role != AuxRole::None && !p.IsAux(i))
            p.other.push_back(i);
    }
    return p;
}

// Which pedals read the pedal device: one set in wheel_pedal_axes does, one set in wheel_axes reads
// the wheel; otherwise the pedal device while one is connected. 0 throttle, 1 brake, 2 clutch.
inline constexpr const char* kPedalKeys[3] = { "throttle", "brake", "clutch" };
struct PedalSources
{
    bool fromAux[3] = { false, false, false };
    bool Any() const { return fromAux[0] || fromAux[1] || fromAux[2]; }
};
inline PedalSources PlanPedals(const std::map<std::string, int>& wheelAxes, const std::map<std::string, int>& pedalAxes,
                               bool auxConnected)
{
    PedalSources out;
    for (int p = 0; p < 3; p++)
    {
        auto set = [&](const std::map<std::string, int>& kv) {
            auto it = kv.find(kPedalKeys[p]);
            return it != kv.end() && it->second >= 0;
        };
        out.fromAux[p] = set(pedalAxes) || (!set(wheelAxes) && auxConnected);
    }
    return out;
}
// Fills the pedals still unknown among `want` with the axes that rest at an end, in axis order
// (a combined pedal axis also counts at the centre, as the first).
inline void DetectPedalAxes(int pedal[3], const bool want[3], int steer, const std::vector<int16_t>& rest, bool combined)
{
    auto filled = [&](int k) { return !want[k] || pedal[k] >= 0; };
    int next = 0;
    for (int a = 0; a < int(rest.size()); a++)
    {
        if (a == steer || a == pedal[0] || a == pedal[1] || a == pedal[2])
            continue;
        if (std::abs(rest[a]) >= 12000 || (combined && next == 0 && !filled(0)))
        {
            while (next < 3 && filled(next))
                next++;
            if (next < 3)
                pedal[next++] = a;
        }
    }
}
// A pedal device's axes: wheel_pedal_axes, then the model's layout, then detection.
inline void PedalDeviceAxes(int pedal[3], const bool want[3], const std::map<std::string, int>& pedalAxes,
                            const AuxModel* model, const std::vector<int16_t>& rest, bool combined)
{
    for (int p = 0; p < 3; p++)
    {
        pedal[p] = -1;
        if (!want[p])
            continue;
        auto it = pedalAxes.find(kPedalKeys[p]);
        if (it != pedalAxes.end() && it->second >= 0)
            pedal[p] = it->second;
        else if (model && model->role == AuxRole::Pedals && model->axes > 0 && int(rest.size()) == model->axes)
            pedal[p] = p == 0 ? model->throttle : p == 1 ? model->brake : model->clutch;
    }
    DetectPedalAxes(pedal, want, -1, rest, combined);
}
// A shifter device's gear buttons: the known model's, then wheel_shifter_device_buttons.
inline ShifterMap AuxShifterMap(const AuxModel* model, const std::map<std::string, int>& overrides)
{
    ShifterMap m;
    if (model && model->role == AuxRole::Shifter && model->firstGear >= 0)
    {
        for (int g = 1; g <= std::min(model->gears, kMaxGear); g++)
            m.gear[g] = model->firstGear + g - 1;
        m.gear[0] = model->reverse;
        m.up = model->up;
        m.down = model->down;
    }
    return ApplyShifterOverrides(m, overrides);
}

// The analog handbrake: wheel_axes "handbrake" on the wheel, else the handbrake device's axis.
enum class HandbrakeSource { None, Wheel, Device };
struct HandbrakePlan
{
    HandbrakeSource source = HandbrakeSource::None;
    int axis = -1;
};
inline HandbrakePlan PlanHandbrake(const std::map<std::string, int>& wheelAxes, bool deviceConnected, int deviceAxis)
{
    const auto it = wheelAxes.find("handbrake");
    if (it != wheelAxes.end() && it->second >= 0)
        return { HandbrakeSource::Wheel, it->second };
    if (deviceConnected && deviceAxis >= 0)
        return { HandbrakeSource::Device, deviceAxis };
    return {};
}
// A device's resting axes with the handbrake's taken out of pedal detection.
inline std::vector<int16_t> RestWithout(const std::vector<int16_t>& rest, int axis)
{
    std::vector<int16_t> out = rest;
    if (axis >= 0 && axis < int(out.size()))
        out[axis] = 0;
    return out;
}
inline int16_t HandbrakeThumb(float h) { return int16_t(std::lround(std::clamp(h, 0.0f, 1.0f) * 32767.0f)); }

// Which joysticks are wheels, the wheel first: the one wheel_device names, then those with force
// feedback, then those the system lists as wheels, then the system's order. A pedal, shifter or
// handbrake device is no wheel unless wheel_device names it.
inline int WheelDeviceMatch(const std::string& wheelDevice, const JoyInfo& j)
{
    return ParseDeviceChoice(wheelDevice) == DeviceChoice::Named ? DeviceMatch(wheelDevice, j) : 0;
}
inline std::vector<int> WheelOrder(const std::vector<JoyInfo>& joys, const std::vector<bool>& wheelLike,
                                   const std::vector<bool>& ffb, const std::vector<bool>& aux, const std::string& wheelDevice)
{
    std::vector<int> out;
    for (int i = 0; i < int(joys.size()); i++)
        if (WheelDeviceMatch(wheelDevice, joys[i]) > 0 || (wheelLike[i] && !aux[i]))
            out.push_back(i);
    auto key = [&](int i) { return WheelDeviceMatch(wheelDevice, joys[i]) * 4 + (ffb[i] ? 2 : 0) + (joys[i].driving ? 1 : 0); };
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return key(a) > key(b); });
    return out;
}

} // namespace wheel
