#pragma once
// DIAGNOSTIC audio capture, off unless FM2_AUDIO_CAPTURE_DIR=<dir> is set (then every call below is a test of one bool).
//   mix.wav        the final 5.1 float mix as handed to the output device, 48 kHz (the header sizes are kept up to date once a second)
//   ctx_<addr>.pcm each XMA context's decoded 16-bit little-endian PCM, as written to its output ring (mono, or stereo interleaved)
//   ctx_events.csv mix_sample,ctx,event,ring_write,pcm_samples,detail: what happened to the contexts and when, in mix samples
// All writing happens on a thread of its own at low priority; a queue bounded to 256 MB drops what it cannot take (counted once in the
// log), and the whole capture stops at 4 GB.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "log.h"

namespace audiocap
{
inline bool On()
{
    static const bool on = getenv("FM2_AUDIO_CAPTURE_DIR") != nullptr && *getenv("FM2_AUDIO_CAPTURE_DIR");
    return on;
}

// Mix samples (per channel) handed to the output device so far: the clock the events are stamped with.
inline std::atomic<uint64_t> g_mixSamples{ 0 };

namespace detail
{
constexpr uint64_t kCapBytes = 4ull << 30, kQueueLimit = 256ull << 20;

struct Item
{
    enum Kind { kMix, kPcm, kEvent } kind = kEvent;
    uint32_t ctx = 0;
    std::vector<uint8_t> bytes;
};

struct State
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    uint64_t queuedBytes = 0;
    std::atomic<uint64_t> writtenBytes{ 0 }, dropped{ 0 };
    std::atomic<bool> stopped{ false };
    bool droppedLogged = false;
    std::map<uint32_t, uint64_t> pcmSamples; // per context: samples queued to its file so far
    std::once_flag started;
};

inline State& S()
{
    static State s;
    return s;
}

inline void WriteWavHeader(FILE* f, uint64_t dataBytes)
{
    const uint32_t rate = 48000, channels = 6, bytesPerSec = rate * channels * 4, fmtSize = 16;
    const uint16_t fmt = 3, ch = channels, align = channels * 4, bits = 32;
    const uint32_t data = uint32_t(std::min<uint64_t>(dataBytes, 0xFFFFFF00ull)), riff = data + 36;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmtSize, 4, 1, f);
    fwrite(&fmt, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&bytesPerSec, 4, 1, f);
    fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
}

inline void Writer()
{
    SetThreadDescription(GetCurrentThread(), L"audio capture writer");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    State& s = S();
    const std::filesystem::path dir = getenv("FM2_AUDIO_CAPTURE_DIR");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    FILE* mix = fopen((dir / "mix.wav").string().c_str(), "wb");
    FILE* events = fopen((dir / "ctx_events.csv").string().c_str(), "wb");
    std::map<uint32_t, FILE*> pcm;
    uint64_t mixBytes = 0;
    if (mix)
        WriteWavHeader(mix, 0);
    if (events)
        fputs("mix_sample,ctx,event,ring_write,pcm_samples,detail\r\n", events);
    auto lastPatch = std::chrono::steady_clock::now();
    for (;;)
    {
        std::deque<Item> batch;
        {
            std::unique_lock lock(s.mutex);
            s.cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return !s.queue.empty(); });
            batch.swap(s.queue);
            s.queuedBytes = 0;
        }
        for (Item& it : batch)
        {
            if (s.writtenBytes.load() + it.bytes.size() > kCapBytes)
            {
                if (!s.stopped.exchange(true))
                    Log("audio capture: the 4 GB cap is reached, capturing stops");
                continue;
            }
            s.writtenBytes += it.bytes.size();
            if (it.kind == Item::kMix && mix)
                fwrite(it.bytes.data(), 1, it.bytes.size(), mix), mixBytes += it.bytes.size();
            else if (it.kind == Item::kEvent && events)
                fwrite(it.bytes.data(), 1, it.bytes.size(), events);
            else if (it.kind == Item::kPcm)
            {
                FILE*& f = pcm[it.ctx];
                if (!f)
                {
                    char name[48];
                    snprintf(name, sizeof name, "ctx_%08X.pcm", it.ctx);
                    f = fopen((dir / name).string().c_str(), "wb");
                }
                if (f)
                    fwrite(it.bytes.data(), 1, it.bytes.size(), f);
            }
        }
        if (std::chrono::steady_clock::now() - lastPatch >= std::chrono::seconds(1))
        {
            lastPatch = std::chrono::steady_clock::now();
            if (mix)
            {
                WriteWavHeader(mix, mixBytes);
                fseek(mix, 0, SEEK_END);
                fflush(mix);
            }
            if (events)
                fflush(events);
            for (auto& [ctx, f] : pcm)
                if (f)
                    fflush(f);
        }
    }
}

inline void Push(Item&& item)
{
    State& s = S();
    if (s.stopped)
        return;
    std::call_once(s.started, [] { std::thread(Writer).detach(); });
    std::lock_guard lock(s.mutex);
    if (s.queuedBytes + item.bytes.size() > kQueueLimit)
    {
        s.dropped++;
        if (!s.droppedLogged)
        {
            s.droppedLogged = true;
            Log("audio capture: the writer cannot keep up, data is being dropped");
        }
        return;
    }
    s.queuedBytes += item.bytes.size();
    s.queue.push_back(std::move(item));
    s.cv.notify_one();
}
} // namespace detail

// One title frame of the final mix (interleaved 5.1 floats), and the mix clock moves on by `frames` samples per channel.
inline void Mix(const float* interleaved, size_t frames, size_t channels)
{
    detail::Item it;
    it.kind = detail::Item::kMix;
    it.bytes.resize(frames * channels * sizeof(float));
    memcpy(it.bytes.data(), interleaved, it.bytes.size());
    detail::Push(std::move(it));
    g_mixSamples += frames;
}

// A context's decoded samples, exactly as written to its output ring.
inline void Pcm(uint32_t ctx, const int16_t* samples, size_t count)
{
    {
        std::lock_guard lock(detail::S().mutex);
        detail::S().pcmSamples[ctx] += count;
    }
    detail::Item it;
    it.kind = detail::Item::kPcm;
    it.ctx = ctx;
    it.bytes.resize(count * sizeof(int16_t));
    memcpy(it.bytes.data(), samples, it.bytes.size());
    detail::Push(std::move(it));
}

// How many samples have gone to the context's .pcm so far (the file offset, in samples, of what the next Pcm call adds).
inline uint64_t PcmSamples(uint32_t ctx)
{
    std::lock_guard lock(detail::S().mutex);
    const auto it = detail::S().pcmSamples.find(ctx);
    return it == detail::S().pcmSamples.end() ? 0 : it->second;
}

// A line in ctx_events.csv: stamped with the mix clock and the context's .pcm position. `detail` must not contain commas.
inline void Event(uint32_t ctx, const char* event, uint32_t ringWrite, const char* fmt, ...)
{
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    char line[560];
    const int n = snprintf(line, sizeof line, "%llu,%08X,%s,%u,%llu,%s\r\n", (unsigned long long)g_mixSamples.load(), ctx, event, ringWrite,
                           (unsigned long long)PcmSamples(ctx), text);
    detail::Item it;
    it.kind = detail::Item::kEvent;
    it.bytes.assign(line, line + std::max(n, 0));
    detail::Push(std::move(it));
}
} // namespace audiocap
