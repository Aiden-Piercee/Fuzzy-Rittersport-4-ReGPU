// The Xbox 360 wheel's force feedback as the Forza engine drives it through the kernel's XInputdFF*
// calls (xboxkrnl ordinals 0x282-0x289, src/kernel/imports_ff.cpp): effect blocks, their state and
// what they ask of the wheel, without any device API (wheel_system.cpp plays them on DirectInput).
//
// Ported from Forza Horizon 1: ReImagined (its SDK patches 0101 wheel_ff.h, 0129 wheel_rumble.h,
// 0131 wheel_min_force.h; BSD 3-Clause, see NOTICE-wheel.md). What FH1 traced in the engine, which FM4
// shares (FM4 looks the same eight kernel functions up at start):
//  - GetDeviceInfo's second word (effects the device plays at once) must be at least 5;
//  - the game stops everything with DeviceControl(3), defines effect blocks 1-5 with one SetEffect and
//    starts them with EffectOperation({block, 2}...): inertia, friction, damper and spring (scaled by
//    Media\ControllerFFB.ini and the wheel options' force feedback scale) and a constant force;
//  - each frame it updates the constant force's magnitude (UpdateEffect flags 0x400) with the car's
//    steering force, and the spring's gain (0x100) with the speed and the rear wheels' slip;
//  - its rumble goes to SetRumble, a 360 controller's two motor speeds.
// An effect is 29 big-endian words: block (1-based), type, a word set to 4, gain (0-255), seven fixed
// words, then the type's parameters from word 11. Types follow HID PID: 1 constant, 8 spring, 9
// damper, 10 inertia, 11 friction.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace wheel
{

constexpr uint32_t kFfConstant = 1;
constexpr uint32_t kFfSpring = 8;
constexpr uint32_t kFfDamper = 9;
constexpr uint32_t kFfInertia = 10;
constexpr uint32_t kFfFriction = 11;

constexpr size_t kFfEffectWords = 29; // 0x74 bytes
constexpr uint32_t kFfMaxBlocks = 16; // what GetDeviceInfo reports; the game uses 1-5

constexpr uint32_t kFfOpStop = 3; // EffectOperation: 2 starts (1 is taken as a start too)
constexpr uint32_t kFfEnableActuators = 1, kFfDisableActuators = 2, kFfStopAll = 3, kFfReset = 4, kFfPause = 5,
                   kFfContinue = 6; // DeviceControl

struct FfEffect
{
    uint32_t block = 0;
    uint32_t type = 0;
    uint32_t gain = 0; // 0-255
    // Constant: word 11 is the magnitude, -127 to 127. Conditions: centre, coefficients on the
    // positive and negative side (-127 to 127), saturations on each side and the dead band (0-255).
    int32_t magnitude = 0;
    int32_t center = 0;
    int32_t posCoeff = 0, negCoeff = 0;
    int32_t posSat = 0, negSat = 0;
    int32_t deadband = 0;
};

inline uint32_t FfWord(const uint8_t* be, size_t i)
{
    const uint8_t* p = be + 4 * i;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

inline FfEffect DecodeFfEffect(const uint8_t* be)
{
    FfEffect e;
    e.block = FfWord(be, 0);
    e.type = FfWord(be, 1);
    e.gain = FfWord(be, 3);
    e.magnitude = e.center = int32_t(FfWord(be, 11));
    e.posCoeff = int32_t(FfWord(be, 12));
    e.negCoeff = int32_t(FfWord(be, 13));
    e.posSat = int32_t(FfWord(be, 14));
    e.negSat = int32_t(FfWord(be, 15));
    e.deadband = int32_t(FfWord(be, 16));
    return e;
}

inline bool FfIsCondition(uint32_t type)
{
    return type == kFfSpring || type == kFfDamper || type == kFfInertia || type == kFfFriction;
}

// What the game has asked of the device: its effect blocks and whether each plays.
class FfState
{
public:
    bool Define(const FfEffect& e)
    {
        if (e.block < 1 || e.block > kFfMaxBlocks)
            return false;
        fx_[e.block - 1] = e;
        defined_[e.block - 1] = true;
        return true;
    }
    bool Operate(uint32_t block, uint32_t op)
    {
        if (block < 1 || block > kFfMaxBlocks || !defined_[block - 1])
            return false;
        playing_[block - 1] = op != kFfOpStop;
        return true;
    }
    void Control(uint32_t command)
    {
        switch (command)
        {
        case kFfEnableActuators: actuators_ = true; break;
        case kFfDisableActuators: actuators_ = false; break;
        case kFfStopAll: std::fill(playing_, playing_ + kFfMaxBlocks, false); break;
        case kFfReset:
            std::fill(playing_, playing_ + kFfMaxBlocks, false);
            std::fill(defined_, defined_ + kFfMaxBlocks, false);
            paused_ = false;
            actuators_ = true;
            break;
        case kFfPause: paused_ = true; break;
        case kFfContinue: paused_ = false; break;
        default: break;
        }
    }
    void SetDeviceGain(uint32_t gain) { deviceGain_ = std::min<uint32_t>(gain, 255); }
    bool Defined(uint32_t block) const { return block >= 1 && block <= kFfMaxBlocks && defined_[block - 1]; }
    const FfEffect& Effect(uint32_t block) const { return fx_[block - 1]; }
    bool Active(uint32_t block) const { return Defined(block) && playing_[block - 1] && actuators_ && !paused_; }
    uint32_t DeviceGain() const { return deviceGain_; }

private:
    FfEffect fx_[kFfMaxBlocks]{};
    bool defined_[kFfMaxBlocks]{};
    bool playing_[kFfMaxBlocks]{};
    bool actuators_ = true;
    bool paused_ = false;
    uint32_t deviceGain_ = 255;
};

// The player's strengths, 0 to 1: overall scales everything, the others one part on top of it.
struct FfStrengths
{
    float overall = 0.6f; // wheel_ffb_strength
    float spring = 1.0f;  // wheel_ffb_spring_strength: the centring spring and the damper
    float engine = 0.5f;  // wheel_rumble_engine_strength: the right motor (mostly the engine)
    float road = 0.8f;    // wheel_rumble_road_strength: the left motor (road, kerbs, impacts)
};

inline float FfTypeStrength(uint32_t type, const FfStrengths& s)
{
    return s.overall * ((type == kFfSpring || type == kFfDamper) ? s.spring : 1.0f);
}

// How strongly an effect plays, 0 to 1: its gain, the device gain and the player's strength.
inline float FfScale(const FfEffect& e, uint32_t deviceGain, float strength)
{
    const float g = std::min<uint32_t>(e.gain, 255) / 255.0f * std::min<uint32_t>(deviceGain, 255) / 255.0f;
    return std::clamp(g * strength, 0.0f, 1.0f);
}

// The constant force on the steering axis, -1 to 1: positive turns the wheel to the right.
inline float FfConstantLevel(const FfEffect& e, float scale)
{
    return std::clamp(e.magnitude / 127.0f, -1.0f, 1.0f) * scale;
}

// Minimum force (wheel_ffb_min_strength): a force x below the minimum m plays as x (1 + 3 (1 - x/m)^2),
// up to 4 times as strong near 0, smooth and rising, and as the game sends it from m up.
inline float MinForce(float x, float min)
{
    x = std::clamp(x, 0.0f, 1.0f);
    if (!(min > 0.0f) || x >= min)
        return x;
    const float u = 1.0f - x / std::min(min, 1.0f);
    return x * (1.0f + 3.0f * u * u);
}
inline float FfMinForceLevel(const FfEffect& e, uint32_t deviceGain, float level, float min)
{
    const float game = std::fabs(FfConstantLevel(e, FfScale(e, deviceGain, 1.0f)));
    if (!(game > 0.0f) || !(min > 0.0f))
        return level;
    return std::clamp(level * (MinForce(game, min) / game), -1.0f, 1.0f);
}

// A condition on the steering axis: coefficients -1 to 1 (positive pulls back to the centre),
// saturations, dead band and centre as fractions of the full range.
struct FfCondition
{
    float rightCoeff = 0, leftCoeff = 0;
    float rightSat = 0, leftSat = 0;
    float deadband = 0;
    float center = 0;
};
inline FfCondition FfConditionOf(const FfEffect& e, float scale)
{
    FfCondition c;
    c.rightCoeff = std::clamp(e.posCoeff / 127.0f, -1.0f, 1.0f) * scale;
    c.leftCoeff = std::clamp(e.negCoeff / 127.0f, -1.0f, 1.0f) * scale;
    c.rightSat = std::clamp(e.posSat / 255.0f, 0.0f, 1.0f) * scale;
    c.leftSat = std::clamp(e.negSat / 255.0f, 0.0f, 1.0f) * scale;
    c.deadband = std::clamp(e.deadband / 255.0f, 0.0f, 1.0f);
    c.center = std::clamp(e.center / 127.0f, -1.0f, 1.0f);
    return c;
}
inline bool FfConditionFelt(const FfCondition& c)
{
    return (c.rightCoeff != 0 && c.rightSat > 0) || (c.leftCoeff != 0 && c.leftSat > 0);
}

// The game's two rumble motors on the wheel's motor. Fine (default): a light, quick sine for each
// motor, the road (left) at 40 Hz and the engine (right) at ~71 Hz, each at most a quarter of the
// strength. Classic: one 25 Hz sine at the stronger motor's level, up to 60 percent.
enum class RumbleStyle { Classic, Fine };
inline RumbleStyle ParseRumbleStyle(const std::string& s) { return s == "classic" ? RumbleStyle::Classic : RumbleStyle::Fine; }
struct RumbleWave
{
    uint32_t periodMs = 0;
    float level = 0.0f;
};
constexpr uint32_t kClassicPeriodMs = 40;
constexpr float kClassicGain = 0.6f;
constexpr uint32_t kFineRoadPeriodMs = 25;
constexpr uint32_t kFineEnginePeriodMs = 14;
constexpr float kFineGain = 0.25f;
inline RumbleWave ClassicRumble(float leftMotor, float rightMotor, const FfStrengths& s)
{
    const float level = std::clamp(std::max(leftMotor * s.road, rightMotor * s.engine) * s.overall, 0.0f, 1.0f);
    return { kClassicPeriodMs, level * kClassicGain };
}
struct FineRumble
{
    RumbleWave road, engine;
};
inline FineRumble FineRumbleOf(float leftMotor, float rightMotor, const FfStrengths& s)
{
    auto level = [&](float motor, float strength) { return std::clamp(motor * strength * s.overall, 0.0f, 1.0f) * kFineGain; };
    return { { kFineRoadPeriodMs, level(leftMotor, s.road) }, { kFineEnginePeriodMs, level(rightMotor, s.engine) } };
}

// The vibration is updated about 30 times a second whatever the frame rate, each time with the root
// mean square of the game's motor levels since the last update (FH1 #467: a wheel that restarts its
// sine at every update buzzed at the frame rate).
constexpr double kRumbleUpdateMs = 33.0;
struct RumbleAverage
{
    double sum[2] = {}; // squares of the left and right motor, 0..1
    int count = 0;
    void Add(float left, float right)
    {
        sum[0] += double(left) * left;
        sum[1] += double(right) * right;
        count++;
    }
    void Take(float& left, float& right)
    {
        left = count ? float(std::sqrt(sum[0] / count)) : 0.0f;
        right = count ? float(std::sqrt(sum[1] / count)) : 0.0f;
        *this = RumbleAverage{};
    }
};

} // namespace wheel
