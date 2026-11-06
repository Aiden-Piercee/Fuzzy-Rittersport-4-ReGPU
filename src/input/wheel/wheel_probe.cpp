// fm4.exe --wheel-probe: the launcher's view of the racing wheel input (its Wheel stuff tab). It
// runs the game's own wheel code (wheel_system.cpp), so the launcher shows the same devices, the same
// axis and button numbers and the same result the game would get.
//
// stdout, one JSON object per line:
//   {"t":"devices","list":[...]}   whenever the device list changes
//   {"t":"state",...}              about 30 times a second: every device's axes, buttons and hats,
//                                  where each control of the wheel reads, and the pad the game gets
// stdin, one line each: "key=value" sets a wheel_* setting, "." applies the settings sent since the
// last "." (the whole set each time), "fftest" plays the steering force right then left (its log
// lines come on stderr), "quit" (or the end of stdin) exits.
#include "input/wheel/wheel_system.h"

#include <atomic>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace wheel
{

namespace
{
std::string Esc(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
            o += '\\', o += char(c);
        else if (c < 0x20)
        {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        }
        else
            o += char(c);
    }
    return o + "\"";
}

std::string Src(const Source& s) { return "[" + std::to_string(s.dev) + "," + std::to_string(s.index) + "]"; }

std::string Num(float f)
{
    char b[32];
    snprintf(b, sizeof(b), "%.4f", f);
    return b;
}

std::mutex g_out;
void Line(const std::string& s)
{
    std::lock_guard l(g_out);
    fwrite(s.data(), 1, s.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void LogToStderr(const char* line)
{
    std::lock_guard l(g_out);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}
} // namespace

int ProbeMain()
{
    timeBeginPeriod(1);
    SetLog(LogToStderr);
    std::map<std::string, std::string> settings;
    auto config = [&] { return Config::From([&](const char* k) { auto it = settings.find(k); return it == settings.end() ? std::string() : it->second; }); };
    System& sys = System::Get();
    sys.Start(config());

    std::atomic<bool> quit{ false };
    std::mutex cfgMutex;
    std::thread([&] {
        std::map<std::string, std::string> pending;
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == "quit")
                break;
            if (line == "fftest") // the launcher's Test force feedback: right, then left
            {
                sys.FfTest();
                continue;
            }
            if (line == ".")
            {
                {
                    std::lock_guard l(cfgMutex);
                    settings = pending;
                }
                sys.SetConfig(config());
                pending.clear();
                continue;
            }
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                pending[line.substr(0, eq)] = line.substr(eq + 1);
        }
        quit = true;
    }).detach();

    Line("{\"t\":\"hello\",\"version\":1}");
    uint32_t lastSerial = 0;
    std::vector<DeviceView> devices;
    std::vector<RawView> raw;
    PlanView plan;
    Pad pad;
    Readout ro;
    while (!quit)
    {
        uint32_t serial = 0;
        sys.Snapshot(serial, devices, raw, plan, pad, ro);
        if (serial != lastSerial)
        {
            lastSerial = serial;
            std::string s = "{\"t\":\"devices\",\"list\":[";
            for (size_t i = 0; i < devices.size(); i++)
            {
                const DeviceView& d = devices[i];
                char vp[16];
                snprintf(vp, sizeof(vp), "%04x:%04x", d.vid, d.pid);
                s += i ? "," : "";
                s += "{\"name\":" + Esc(d.name) + ",\"guid\":" + Esc(d.guid) + ",\"id\":" + Esc(d.vid ? vp : "") +
                     ",\"buttons\":" + std::to_string(d.buttons) + ",\"hats\":" + std::to_string(d.hats) + ",\"axes\":[";
                for (size_t a = 0; a < d.axes.size(); a++)
                    s += (a ? "," : "") + Esc(d.axes[a]);
                s += "],\"rest\":[";
                for (size_t a = 0; a < d.rest.size(); a++)
                    s += (a ? "," : "") + std::to_string(d.rest[a]);
                s += "],\"profile\":" + std::string(d.profile ? "true" : "false");
                s += ",\"ffb\":" + std::string(d.ffb ? "true" : "false") + ",\"driving\":" + (d.driving ? "true" : "false") +
                     ",\"wheelLike\":" + (d.wheelLike ? "true" : "false") + ",\"role\":" + Esc(d.role) + ",\"model\":" + Esc(d.model) + "}";
            }
            Line(s + "]}");
        }
        std::string s = "{\"t\":\"state\",\"raw\":[";
        for (size_t i = 0; i < raw.size(); i++)
        {
            const RawView& r = raw[i];
            s += i ? ",{\"a\":[" : "{\"a\":[";
            for (size_t a = 0; a < r.axes.size(); a++)
                s += (a ? "," : "") + std::to_string(r.axes[a]);
            s += "],\"b\":\"";
            for (uint8_t b : r.buttons)
                s += b ? '1' : '0';
            s += "\",\"h\":[";
            for (size_t h = 0; h < r.hats.size(); h++)
                s += (h ? "," : "") + std::to_string(r.hats[h]);
            s += "]}";
        }
        s += "],\"use\":[";
        for (size_t i = 0; i < devices.size(); i++)
            s += (i ? "," : "") + Esc(devices[i].use);
        s += "],\"plan\":{\"wheel\":" + std::to_string(plan.wheel) + ",\"pedals\":" + std::to_string(plan.pedals) +
             ",\"shifter\":" + std::to_string(plan.shifter) + ",\"handbrakeDev\":" + std::to_string(plan.handbrakeDev) +
             ",\"steer\":" + Src(plan.steer) + ",\"throttle\":" + Src(plan.pedal[0]) + ",\"brake\":" + Src(plan.pedal[1]) +
             ",\"clutch\":" + Src(plan.pedal[2]) + ",\"handbrake\":" + Src(plan.handbrake) +
             ",\"combined\":" + (plan.combined ? "true" : "false") + ",\"buttons\":{";
        for (size_t i = 0; i < plan.buttons.size(); i++)
            s += (i ? "," : "") + Esc(plan.buttons[i].first) + ":" + Src(plan.buttons[i].second);
        s += "},\"gears\":{";
        for (size_t i = 0; i < plan.gears.size(); i++)
            s += (i ? "," : "") + Esc(plan.gears[i].first) + ":" + Src(plan.gears[i].second);
        s += "}},\"pad\":{\"buttons\":" + std::to_string(pad.buttons) + ",\"lt\":" + std::to_string(pad.lt) + ",\"rt\":" +
             std::to_string(pad.rt) + ",\"lx\":" + std::to_string(pad.lx) + ",\"ly\":" + std::to_string(pad.ly) + ",\"rx\":" +
             std::to_string(pad.rx) + ",\"ry\":" + std::to_string(pad.ry) + "},\"out\":{\"steer\":" + Num(ro.steer) +
             ",\"throttle\":" + Num(ro.throttle) + ",\"brake\":" + Num(ro.brake) + ",\"clutch\":" + Num(ro.clutch) +
             ",\"handbrake\":" + Num(ro.handbrake) + ",\"lever\":" + std::to_string(ro.lever) + ",\"gear\":" + std::to_string(ro.gear) + "}}";
        Line(s);
        Sleep(33);
    }
    return 0;
}

} // namespace wheel
