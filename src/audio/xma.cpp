// XMA decoder contexts. The title (FMOD) drives the console's hardware decoder
// through the kernel's XMA* exports, which edit a 64-byte context in guest
// memory: two input buffers of 2 KB XMA packets, an output ring of 256-byte
// blocks of 16-bit big-endian PCM, loop points. Each context gets an FFmpeg
// xma2 decoder fed one packet at a time. Like the hardware it runs ahead
// whenever there is output space; the consumer sets the pace.
//
// XMA_CONTEXT_DATA, big-endian dwords, bit fields counted from bit 0:
//   0: in0 packet count 0-11, loop count 12-19, in0 valid 20, in1 valid 21,
//      output block count 22-26, output write offset 27-31
//   1: in1 packet count 0-11, loop subframe end 14-16, skip 17-19,
//      subframe decode count 20-23, sample rate 27-28, stereo 29,
//      output valid 31
//   2: input read offset (bits) 0-25
//   3: loop start 0-25      4: loop end 0-25, current input buffer 31
//   5: in0 address  6: in1 address  7: output address  8: work buffer
//   9: output read offset 0-4
#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <windows.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
}

#include "kernel/imports.h"
#include "live_switch.h"
#include "log.h"

bool DealerMutesMusic(); // hooks/dealer.cpp: the easter egg is playing

namespace
{
constexpr uint32_t kContextSize = 0x40;
constexpr uint32_t kPacketBits = 2048 * 8;
constexpr uint32_t kBlockBytes = 256;
constexpr uint32_t kPacketBytes = 2048;

const bool g_traceXma = getenv("FM2_TRACE_XMA") != nullptr;

// Counted always, logged every 5 s while they change (see ReportStats).
std::atomic<uint64_t> g_decodeErrors{ 0 }, g_guardFlushes{ 0 }, g_packetsDecoded{ 0 };
std::atomic<int> g_traceBudget{ 200 }; // FM2_TRACE_XMA lines for guard flushes and decode errors

// DIAGNOSTIC (always on, cheap): which decoders FFmpeg complains about, what was fed them just before, and how the
// title's contexts change identity. Counted in the 5 s line; the first few offenders are written to files.
std::atomic<uint64_t> g_errorPackets{ 0 }, g_dumps{ 0 }, g_bufferChanges{ 0 }, g_rateChanges{ 0 }, g_offsetWrites{ 0 };
std::atomic<uint64_t> g_regKicks{ 0 }, g_regLocks{ 0 }, g_regClears{ 0 };
std::atomic<int> g_idBudget{ 300 }; // lines for identity changes and clears
constexpr uint32_t kRingPackets = 16, kMaxDumps = 40;
thread_local bool t_errorSeen = false; // FFmpeg logged an error on this thread since it was last cleared

// FFmpeg's messages: flags the error for the packet being decoded, and prints at most 20 lines a second.
void XmaLogCallback(void* avcl, int level, const char* fmt, va_list vl)
{
    if (level > AV_LOG_ERROR)
        return;
    char text[256];
    va_list copy;
    va_copy(copy, vl);
    vsnprintf(text, sizeof text, fmt, copy);
    va_end(copy);
    if (!strstr(text, "timestamps"))
        t_errorSeen = true;
    static std::atomic<int64_t> second{ 0 };
    static std::atomic<int> printed{ 0 };
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (second.exchange(now) != now)
        printed = 0;
    if (printed++ < 20)
        av_log_default_callback(avcl, level, fmt, vl);
}

uint32_t Field(uint32_t ctx, int dword, int lo, int bits)
{
    return (LoadBE32(ctx + dword * 4) >> lo) & ((1u << bits) - 1);
}

void SetField(uint32_t ctx, int dword, int lo, int bits, uint32_t value)
{
    const uint32_t mask = ((1u << bits) - 1) << lo;
    const uint32_t a = ctx + dword * 4;
    StoreBE32(a, (LoadBE32(a) & ~mask) | ((value << lo) & mask));
}

// A buffer address in a context, as guest memory. The decoder takes physical addresses (29 bits),
// as titles that write contexts themselves store them (FM4's FMOD: MmGetPhysicalAddress of its
// buffers); they are read through the 0xA0000000 physical view.
uint32_t ContextBuffer(uint32_t address)
{
    return 0xA0000000 + (address & 0x1FFFFFFF);
}

// XMAInitializeContext takes the title's (virtual) addresses and stores physical ones, as the
// kernel does. XMA buffers are physical allocations, so they are in a physical view.
uint32_t ToContextBuffer(uint32_t address)
{
    if (address && address < 0xA0000000)
        LOG_ONCE("xma: buffer %08X is not in a physical view", address);
    return address & 0x1FFFFFFF;
}

std::mutex g_mutex;

// Music packets, by a hash of their first 64 bytes: the title reads its music files (Media/Audio/Music/*.xma)
// into a file buffer and copies packets on to the decoder's input, so a packet is recognised by
// its contents (AudioNoteMusicRead). The "whysthisdealer" easter egg silences music while it plays.
std::mutex g_musicMutex;
std::set<uint64_t> g_musicPackets;
uint64_t PacketHash(const uint8_t* p)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 64; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
bool MusicPacket(const uint8_t* p)
{
    std::lock_guard lock(g_musicMutex);
    return g_musicPackets.count(PacketHash(p)) != 0;
}
std::set<uint32_t> g_enabled;
std::once_flag g_started;

// The hardware's contexts: one array of 320 in physical memory, whose physical address the APU
// register ContextArrayAddress holds. XMACreateContext hands them out; a context's hardware ID is
// its index, and the Kick/Lock/Clear registers take one bit per ID (32 per register).
constexpr uint32_t kContextCount = 320;
uint32_t g_contextArray = 0;
bool g_contextUsed[kContextCount] = {};

// APU registers (guest 0x7FEA0000, register r at + r * 4), as titles that drive the decoder
// directly use them (FM4's FMOD and XAudio code: they import no XMAEnableContext).
constexpr uint32_t kApuBase = 0x7FEA0000;
constexpr uint32_t kRegContextArrayAddress = 0x600;
constexpr uint32_t kRegNextContextIndex = 0x607;
constexpr uint32_t kRegKick = 0x650, kRegLock = 0x690, kRegClear = 0x6A0; // 10 registers each
uint32_t g_registers[0x10000 / 4] = {};

// Registers are little-endian: a register's value sits in guest memory as the title reads it
// with lwbrx. Titles read these with plain loads, so the memory copy is what they see.
void SetRegister(uint32_t r, uint32_t value)
{
    g_registers[r] = value;
    *GuestPtr<uint32_t>(kApuBase + r * 4) = value;
}

AVCodecContext* OpenCodec(uint32_t ch, uint32_t hz)
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext* av = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!av)
        return nullptr;
    av->sample_rate = int(hz);
    av_channel_layout_default(&av->ch_layout, int(ch));
    av->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX (34 bytes): one stream; FFmpeg reads the stream count from it.
    av->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    av->extradata_size = 34;
    av->extradata[0] = 1;
    if (avcodec_open2(av, codec, nullptr) < 0)
        avcodec_free_context(&av);
    return av;
}

// Codecs are opened and freed only by the decoder thread, outside g_mutex. An XMA2 codec takes a few MB and 1-3 ms
// to open (eight stream decoders and their transforms), and a stream that restarts (an engine sound starting,
// a loop) gets a fresh one. Opened by whatever stepped the stream, that was the title's mixer thread
// (XMAEnableContext decodes the first blocks) or the decoder thread holding g_mutex, which the mixer waits on: at
// a race start the mixer can miss its 1024-sample deadline and the console's driver replays its last block, heard
// as stretched, crusty sound. A stream that needs a codec when none is ready waits for the next pass (2 ms; the
// hardware decodes in the background too), and the mixer polls it meanwhile. Each format keeps as many spares as
// it has taken between two refills at its busiest, from 2 up to 40: memory only once a format has needed it.
// FM2_XMA_NO_CODEC_POOL=1: opened in place.
// (Ported from zer0ducksgiven's Forza-Motorsport-2-ReGPU, 7870b2b and c830cbc.)
constexpr size_t kMinPooledCodecs = 2, kMaxPooledCodecs = 40;
std::mutex g_poolMutex;
std::map<std::pair<uint32_t, uint32_t>, std::vector<AVCodecContext*>> g_codecPool;
std::map<std::pair<uint32_t, uint32_t>, size_t> g_codecsTaken, g_codecTarget; // taken since the last refill, spares kept
std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> g_codecsWanted;   // contexts waiting for one
std::vector<AVCodecContext*> g_retiredCodecs;
std::atomic<uint64_t> g_poolWaits{ 0 }, g_codecReopens{ 0 }; // (counted in the 5 s line)

bool NoCodecPool()
{
    static LiveSwitch noPool("FM2_XMA_NO_CODEC_POOL");
    return noPool;
}

// Whether a codec for ch/hz can be taken now for context ctx. If not, the decoder thread opens one before its next
// pass (one per waiting context: the title polls a waiting one thousands of times).
bool CodecReady(uint32_t ctx, uint32_t ch, uint32_t hz)
{
    if (NoCodecPool())
        return true;
    std::lock_guard lock(g_poolMutex);
    if (!g_codecPool[{ ch, hz }].empty())
        return true;
    g_codecsWanted[{ ch, hz }].insert(ctx);
    g_poolWaits++;
    return false;
}

AVCodecContext* TakeCodec(uint32_t ch, uint32_t hz)
{
    if (!NoCodecPool())
    {
        std::lock_guard lock(g_poolMutex);
        auto& pool = g_codecPool[{ ch, hz }]; // (a new entry: the decoder thread keeps it filled)
        g_codecsTaken[{ ch, hz }]++;
        if (!pool.empty())
        {
            AVCodecContext* av = pool.back();
            pool.pop_back();
            return av;
        }
    }
    return OpenCodec(ch, hz); // (with the pool: a first use, or a caller that didn't ask CodecReady)
}

void RetireCodec(AVCodecContext* av)
{
    if (!av)
        return;
    if (NoCodecPool())
        return avcodec_free_context(&av);
    std::lock_guard lock(g_poolMutex);
    g_retiredCodecs.push_back(av);
}

// On the decoder thread, without g_mutex: frees used codecs, then opens codecs for the streams waiting and up to
// each format's target of spares, all of them (a stream waiting for one is polled by the title's mixer thousands of
// times a second at a race start).
void RefillCodecPool()
{
    std::vector<AVCodecContext*> retired;
    std::vector<std::pair<uint32_t, uint32_t>> wanted;
    {
        std::lock_guard lock(g_poolMutex);
        retired.swap(g_retiredCodecs);
        for (const auto& [key, pool] : g_codecPool)
        {
            size_t& target = g_codecTarget[key];
            const size_t taken = std::min(g_codecsTaken[key], kMaxPooledCodecs);
            if (taken > target && taken > kMinPooledCodecs)
                Log("xma: keeping %zu spare codecs for %u ch %u Hz (%zu taken between refills)", taken, key.first, key.second, g_codecsTaken[key]);
            target = std::max({ target, taken, kMinPooledCodecs });
            g_codecsTaken[key] = 0;
            const size_t waiting = g_codecsWanted.count(key) ? g_codecsWanted[key].size() : 0;
            for (size_t i = pool.size(); i < std::max(target, kMinPooledCodecs) + waiting; i++)
                wanted.push_back(key);
        }
        g_codecsWanted.clear();
    }
    for (AVCodecContext* av : retired)
        avcodec_free_context(&av);
    for (const auto& key : wanted)
        if (AVCodecContext* av = OpenCodec(key.first, key.second))
        {
            std::lock_guard lock(g_poolMutex);
            g_codecPool[key].push_back(av);
        }
}

// A packet as it was fed to a codec, with where it came from.
struct FedPacket
{
    uint32_t bufferAddr = 0, packetIndex = 0, readOffset = 0;
    uint8_t bufferIndex = 0, rateBits = 0, chBit = 0, fresh = 0; // fresh: the first packet of a new codec
    uint8_t data[kPacketBytes];
};

struct FedInfo
{
    uint32_t ctx = 0, bufferAddr = 0, packetIndex = 0, readOffset = 0;
    uint8_t bufferIndex = 0, rateBits = 0, chBit = 0;
};

struct DumpJob
{
    uint32_t seq = 0, ctx = 0, channels = 0, rate = 0;
    std::vector<FedPacket> packets; // oldest first, the failing packet last
};

// Dump files are written by a low-priority thread of their own, never by the decoder or the mixer. FM2_XMA_DUMP_DIR=<dir>
// turns them on: <seq>_<context>.bin (the packets, 2 KB each) and .txt (where each came from).
std::mutex g_dumpMutex;
std::condition_variable g_dumpCv;
std::deque<DumpJob> g_dumpQueue;
std::once_flag g_dumpStarted;

void DumpWriter()
{
    SetThreadDescription(GetCurrentThread(), L"xma dump writer");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const char* dir = getenv("FM2_XMA_DUMP_DIR");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (;;)
    {
        DumpJob job;
        {
            std::unique_lock lock(g_dumpMutex);
            g_dumpCv.wait(lock, [] { return !g_dumpQueue.empty(); });
            job = std::move(g_dumpQueue.front());
            g_dumpQueue.pop_front();
        }
        char name[64];
        snprintf(name, sizeof name, "%03u_%08X", job.seq, job.ctx);
        const std::filesystem::path base = std::filesystem::path(dir) / name;
        if (FILE* f = fopen((base.string() + ".bin").c_str(), "wb"))
        {
            for (const FedPacket& p : job.packets)
                fwrite(p.data, 1, kPacketBytes, f);
            fclose(f);
        }
        if (FILE* t = fopen((base.string() + ".txt").c_str(), "w"))
        {
            fprintf(t, "ctx=%08X ch=%u rate=%u packets=%zu (oldest first; the last one is where FFmpeg complained)\n", job.ctx, job.channels, job.rate, job.packets.size());
            for (size_t i = 0; i < job.packets.size(); i++)
            {
                const FedPacket& p = job.packets[i];
                fprintf(t, "i=%zu buf=%08X bi=%u pkt=%u off=%u ratebits=%u chbit=%u fresh=%u\n", i, p.bufferAddr, p.bufferIndex, p.packetIndex, p.readOffset, p.rateBits,
                        p.chBit, p.fresh);
            }
            fclose(t);
        }
    }
}

// How an XMA2 packet links to the next one: its frames run back to back from the header's first-frame offset, each
// starting with a 15-bit length (in bits, itself included), and the last frame usually runs on into the next packet.
// The next packet of the same stream then starts with a first-frame offset equal to the bits that ran on. A packet
// that does not satisfy that is not the next one of the stream, whatever address or offset it was read from.
unsigned FirstFrameOffset(const uint8_t* p)
{
    return ((uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) >> 11) & 0x7FFF;
}

// A packet with no frame start: the first-frame offset is 0x7FFF and the frame count 0. It carries the middle of one frame that is
// longer than a packet; alone, a codec reads the offset as "skip 32767 bits" and then drops the next packet it is given.
bool NoFrameStart(const uint8_t* p)
{
    return FirstFrameOffset(p) == 0x7FFF && (p[0] >> 2) == 0;
}

// The bits of the packet's last frame that run on into the next packet, or -1 if the packet cannot be walked.
int SpillBits(const uint8_t* p)
{
    constexpr unsigned dataBits = kPacketBits - 32;
    const unsigned frames = (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) >> 26;
    unsigned pos = FirstFrameOffset(p), end = 0, n = 0;
    auto bit = [&](unsigned at) { return (p[4 + (at >> 3)] >> (7 - (at & 7))) & 1; };
    while (n < frames && pos + 15 <= dataBits)
    {
        unsigned len = 0;
        for (int i = 0; i < 15; i++)
            len = len << 1 | unsigned(bit(pos + i));
        if (!len)
            break;
        pos += len, end = pos, n++;
    }
    if (!end)
        return -1;
    return end > dataBits ? int(end - dataBits) : 0;
}

struct Decoder
{
    AVCodecContext* av = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    uint32_t channels = 0, rate = 0;
    std::vector<int16_t> pcm; // decoded, interleaved, not yet in the output ring
    size_t pcmRead = 0;
    bool flush = true; // start of a stream or a jump: reset before the next packet
    bool starved = false;
    // When both input buffers ran out with audio still in the codec. A stream the title feeds a
    // buffer at a time is refilled within milliseconds; only a longer wait is the stream's end.
    Clock::time_point dryAt{};
    size_t packetsDecoded = 0, framesWritten = 0, framesDecoded = 0;
    // An input buffer whose packets are all decoded stays valid for the title until their audio
    // has reached the output ring (the hardware has no backlog, so the title takes "consumed"
    // to mean "delivered"); it would loop or stop a stream while the tail was still queued here.
    bool consumed[2] = {};
    bool holdsTail = false; // packets were decoded since the codec was last drained or reset
    bool seeked = false; // the title set the read offset since the last packet was decoded
    bool music = false;  // its last packet came from a music file (MusicPacket)
    // Where the last packet that decoded came from: input buffer, packet index and the buffer's packet count
    // (lastBuffer -1: none since the codec was opened or reset). Used to tell a read that follows from a jump.
    bool used = false; // packets were sent to this codec: only a fresh one is a real reset
    int lastBuffer = -1;
    uint32_t lastPacket = 0, lastCount = 0;
    uint64_t decodeErrors = 0, guardFlushes = 0, errorPackets = 0;
    // The last packets fed to the codec, newest last in ring order; dumped once when FFmpeg first complains.
    std::array<FedPacket, kRingPackets> ring;
    uint32_t ringNext = 0, ringCount = 0;
    bool dumped = false;
    // What the context looked like when the last packet was fed (identity changes are counted and logged).
    uint32_t lastAddr[2] = {};
    int lastRateBits = -1, lastChBit = -1;
    uint32_t expectedOffset = 0;
    bool haveExpected = false;
    // Bits of the last frame fed that are still to come in the packets after it (a no-frame-start packet takes a whole packet of them);
    // -1 when unknown. The next packet must start its first frame exactly there.
    int pendingBits = -1;

    void Close()
    {
        RetireCodec(av);
        av = nullptr;
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    // A codec from the pool (see CodecReady): opening one takes milliseconds, too long for the title's mixer
    // thread, which steps streams through XMAEnableContext.
    bool Open(uint32_t ch, uint32_t hz)
    {
        RetireCodec(av);
        if (!(av = TakeCodec(ch, hz)))
            return false;
        if (!packet)
            packet = av_packet_alloc();
        if (!frame)
            frame = av_frame_alloc();
        channels = ch;
        rate = hz;
        used = false;
        lastBuffer = -1;
        pendingBits = -1;
        return true;
    }

    // A real reset: FFmpeg's XMA decoder has no flush callback, so avcodec_flush_buffers leaves its overlap, its
    // sample FIFO and its end-of-stream state in place, and the leftover bits of the old position are joined to
    // the next packet ("would have to skip N bits", "invalid number of fill bits", garbage frames). A restarted
    // stream gets a new codec instead, once one is ready (false: wait for the next pass).
    // FM2_XMA_FLUSH_ONLY=1: avcodec_flush_buffers, as before.
    bool Reset(uint32_t ctx)
    {
        static LiveSwitch flushOnly("FM2_XMA_FLUSH_ONLY");
        if (!flush || !used || flushOnly)
            return true; // (nothing sent yet, or the old reset, done in Decode)
        if (!CodecReady(ctx, channels, rate))
            return false;
        RetireCodec(av);
        if (!(av = TakeCodec(channels, rate)))
            return false;
        used = holdsTail = false;
        flush = false;
        lastBuffer = -1;
        pendingBits = -1;
        g_codecReopens++;
        return true;
    }

    // The end of a stream: the codec holds back about a packet of audio until more input
    // comes. Appends it (and silence up to a whole output block) and resets the codec.
    void Drain(size_t block)
    {
        if (!av)
            return;
        const size_t pcmBefore = pcm.size();
        avcodec_send_packet(av, nullptr);
        while (avcodec_receive_frame(av, frame) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = 0; i < n; i++)
                    pcm[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        if (const size_t rest = (pcm.size() - pcmRead) % block)
            pcm.resize(pcm.size() + (block - rest), 0);
        if (g_traceXma)
        {
            int peak = 0;
            for (size_t i = pcmBefore; i < pcm.size(); i++)
                peak = std::max(peak, std::abs(int(pcm[i])));
            Log("xma: drained %zu frames at the end of a stream (peak %d, %u ch %u Hz)", (pcm.size() - pcmBefore) / channels, peak, channels, rate);
        }
        flush = true;
        holdsTail = false;
    }

    // Decodes one 2 KB packet and appends its samples.
    bool Decode(const uint8_t* data, const FedInfo& info)
    {
        const bool wasFlush = flush;
        t_errorSeen = false;
        {
            FedPacket& slot = ring[ringNext % kRingPackets];
            slot.bufferAddr = info.bufferAddr, slot.packetIndex = info.packetIndex, slot.readOffset = info.readOffset;
            slot.bufferIndex = info.bufferIndex, slot.rateBits = info.rateBits, slot.chBit = info.chBit, slot.fresh = !used;
            memcpy(slot.data, data, kPacketBytes);
            ringNext++;
            ringCount = std::min(ringCount + 1, kRingPackets);
        }
        // After FFmpeg has been fed: counts a packet it complained about and queues the dump of the first one of this decoder.
        auto noteErrors = [&] {
            if (!t_errorSeen)
                return;
            t_errorSeen = false;
            errorPackets++, g_errorPackets++;
            if (dumped || !getenv("FM2_XMA_DUMP_DIR") || g_dumps >= kMaxDumps)
                return;
            dumped = true;
            DumpJob job;
            job.seq = uint32_t(++g_dumps), job.ctx = info.ctx, job.channels = channels, job.rate = rate;
            for (uint32_t i = 0; i < ringCount; i++)
                job.packets.push_back(ring[(ringNext - ringCount + i) % kRingPackets]);
            std::call_once(g_dumpStarted, [] { std::thread(DumpWriter).detach(); });
            {
                std::lock_guard lock(g_dumpMutex);
                g_dumpQueue.push_back(std::move(job));
            }
            g_dumpCv.notify_one();
        };
        if (flush)
        {
            if (used)
                avcodec_flush_buffers(av); // (Reset replaces the codec before this unless FM2_XMA_FLUSH_ONLY)
            flush = false;
        }
        used = true;
        const size_t pcmBefore = pcm.size();
        packetsDecoded++;
        if (av_new_packet(packet, kPacketBytes) < 0)
            return false;
        memcpy(packet->data, data, kPacketBytes);
        const int sent = avcodec_send_packet(av, packet);
        av_packet_unref(packet);
        if (sent < 0)
        {
            noteErrors();
            decodeErrors++, g_decodeErrors++;
            flush = true;
            lastBuffer = -1;
            return false;
        }
        int received;
        while ((received = avcodec_receive_frame(av, frame)) == 0)
        {
            const int n = frame->nb_samples, ch = std::min<int>(frame->ch_layout.nb_channels, int(channels));
            const size_t base = pcm.size();
            pcm.resize(base + size_t(n) * channels);
            for (int c = 0; c < int(channels); c++)
            {
                const float* in = reinterpret_cast<const float*>(frame->extended_data[std::min(c, ch - 1)]);
                for (int i = 0; i < n; i++)
                    pcm[base + size_t(i) * channels + c] = int16_t(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
            }
            av_frame_unref(frame);
        }
        noteErrors();
        const bool failed = received != AVERROR(EAGAIN) && received != AVERROR_EOF;
        if (failed)
            decodeErrors++, g_decodeErrors++;
        g_packetsDecoded++;
        framesDecoded += (pcm.size() - pcmBefore) / channels;
        holdsTail = true;
        if (g_traceXma)
        {
            int peak = 0;
            for (size_t i = pcmBefore; i < pcm.size(); i++)
                peak = std::max(peak, std::abs(int(pcm[i])));
            Log("xma: decoded a packet: %zu frames (flush %d, peak %d)", (pcm.size() - pcmBefore) / channels, int(wasFlush), peak);
        }
        return !failed;
    }
};

std::map<uint32_t, Decoder> g_decoders;

// The title refilled an input buffer: whatever "consumed" was still pending for it is over.
void ClearConsumed(uint32_t ctx, int dword, int bit)
{
    if (dword != 0 || (bit != 20 && bit != 21))
        return;
    if (auto it = g_decoders.find(ctx); it != g_decoders.end())
        it->second.consumed[bit - 20] = false;
}

// The output ring has a free block: fill it from decoded samples. Returns false when it
// has nothing decoded to give.
bool WriteBlock(uint32_t ctx, Decoder& d)
{
    const size_t perBlock = kBlockBytes / 2;
    if (d.pcm.size() - d.pcmRead < perBlock)
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5), write = Field(ctx, 0, 27, 5);
    uint16_t* out = GuestPtr<uint16_t>(ContextBuffer(LoadBE32(ctx + 7 * 4)) + write * kBlockBytes);
    const bool mute = d.music && DealerMutesMusic();
    for (size_t i = 0; i < perBlock; i++)
        out[i] = mute ? 0 : __builtin_bswap16(uint16_t(d.pcm[d.pcmRead + i]));
    d.pcmRead += perBlock;
    d.framesWritten += perBlock / std::max<uint32_t>(d.channels, 1);
    if (d.pcmRead > (1u << 16))
    {
        d.pcm.erase(d.pcm.begin(), d.pcm.begin() + ptrdiff_t(d.pcmRead));
        d.pcmRead = 0;
    }
    SetField(ctx, 0, 27, 5, (write + 1) % blocks);
    return true;
}

// One decoder step for a context: fill one output block, decoding the next input packet
// when needed. Returns false when it has nothing to do.
bool Step(uint32_t ctx)
{
    if (!Field(ctx, 1, 31, 1)) // output buffer not valid
        return false;
    const uint32_t blocks = Field(ctx, 0, 22, 5);
    if (!blocks)
        return false;
    const uint32_t write = Field(ctx, 0, 27, 5), read = Field(ctx, 9, 0, 5);
    // Keep one block free so a full ring never reads as empty.
    if ((read + blocks - write - 1) % blocks == 0)
        return false;

    static const uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
    const uint32_t channels = Field(ctx, 1, 29, 1) ? 2 : 1, rate = kRates[Field(ctx, 1, 27, 2)];
    Decoder& d = g_decoders[ctx];
    if (!d.av || d.channels != channels || d.rate != rate)
        if (!CodecReady(ctx, channels, rate) || !d.Open(channels, rate))
            return false;
    if (WriteBlock(ctx, d))
        return true;

    // Everything decoded so far has been handed over. If no input is waiting, this is the end of
    // the stream: deliver what the codec still holds first.
    auto valid = [&](uint32_t index) { return Field(ctx, 0, 20 + index, 1) && !d.consumed[index]; };
    // With both input buffers used up the decoder waits, its state intact, for the title to mark
    // one valid again, as the hardware does; the context's state resets only when the title
    // clears or re-initialises it. (Draining and resetting the codec the moment the input ran
    // dry gave every packet of a stream fed one buffer at a time a cold decoder.)
    // FM2_XMA_DRAIN_MS=<ms> (debug only): drain a stream left dry that long.
    static const int drainMs = getenv("FM2_XMA_DRAIN_MS") ? atoi(getenv("FM2_XMA_DRAIN_MS")) : 0;
    if (drainMs > 0 && d.holdsTail && !valid(0) && !valid(1))
    {
        const auto now = Clock::now();
        if (d.dryAt == Clock::time_point{})
            d.dryAt = now;
        if (now - d.dryAt >= std::chrono::milliseconds(drainMs))
        {
            d.dryAt = {};
            d.Drain(kBlockBytes / 2);
            if (WriteBlock(ctx, d))
                return true;
        }
    }
    else
        d.dryAt = {};
    for (uint32_t i = 0; i < 2; i++)
        if (d.consumed[i])
        {
            d.consumed[i] = false;
            SetField(ctx, 0, 20 + i, 1, 0); // now the title may refill (or loop) it
        }

    uint32_t current = Field(ctx, 4, 31, 1);
    if (!valid(current))
    {
        current ^= 1;
        if (!valid(current))
        {
            if (g_traceXma && !d.starved)
                Log("xma: %08X starved (both input buffers consumed)", ctx);
            d.starved = true;
            return false; // starved: both input buffers consumed
        }
        d.starved = false;
        SetField(ctx, 4, 31, 1, current);
        SetField(ctx, 2, 0, 26, 32);
    }
    const uint32_t packets = current ? Field(ctx, 1, 0, 12) : Field(ctx, 0, 0, 12);
    const uint32_t buffer = ContextBuffer(LoadBE32(ctx + (5 + current) * 4));
    const uint32_t packet = Field(ctx, 2, 0, 26) / kPacketBits;
    const uint32_t bufferAddr = LoadBE32(ctx + (5 + current) * 4), rateBits = Field(ctx, 1, 27, 2), chBit = Field(ctx, 1, 29, 1);
    if (packet < packets)
    {
        // DIAGNOSTIC: the context changing identity under a running decoder.
        {
            const uint32_t offsetNow = Field(ctx, 2, 0, 26);
            if (d.lastAddr[current] && d.lastAddr[current] != bufferAddr)
            {
                g_bufferChanges++;
                if (g_idBudget.fetch_sub(1) > 0)
                    Log("xma: id %08X: input buffer %u moved %08X -> %08X (%u packets)", ctx, current, d.lastAddr[current], bufferAddr, packets);
            }
            d.lastAddr[current] = bufferAddr;
            if (d.lastRateBits >= 0 && (int(rateBits) != d.lastRateBits || int(chBit) != d.lastChBit))
            {
                g_rateChanges++;
                if (g_idBudget.fetch_sub(1) > 0)
                    Log("xma: id %08X: rate/channel bits %d/%d -> %u/%u", ctx, d.lastRateBits, d.lastChBit, rateBits, chBit);
            }
            d.lastRateBits = int(rateBits), d.lastChBit = int(chBit);
            if (d.haveExpected && offsetNow != d.expectedOffset)
            {
                g_offsetWrites++;
                if (g_idBudget.fetch_sub(1) > 0)
                    Log("xma: id %08X: read offset written by the title: %u (expected %u), buffer %u packet %u", ctx, offsetNow, d.expectedOffset, current, packet);
            }
        }
        if (g_traceXma && d.seeked)
        {
            const uint32_t header = LoadBE32(buffer + packet * kPacketBytes), start = Field(ctx, 2, 0, 26) % kPacketBits;
            if (start != 32 + ((header >> 11) & 0x7FFF))
                Log("xma: %08X decode starts mid-packet: packet %u of %u, offset %u in it, the packet's first frame at %u", ctx, packet, packets, start,
                    32 + ((header >> 11) & 0x7FFF));
        }
        d.seeked = false;
        d.music = MusicPacket(GuestPtr<uint8_t>(buffer + packet * kPacketBytes));
        if (d.music)
            LOG_ONCE("xma: %08X is music (the first stream recognised)", ctx);
        // A frame can run on into the next packet, and the codec keeps the unfinished one until that packet
        // arrives. The packet that follows is the next one of the same input buffer, or the first of a buffer
        // after the last packet of the previous one. Anything else (a loop, a restart elsewhere in the stream)
        // must not inherit the old tail: it would be joined to the new packet's start and decode as garbage.
        // Reads that do follow (the pair restarts the title makes) keep the codec state. FM2_NO_XMA_SEEK_GUARD=1: off.
        static LiveSwitch noGuard("FM2_NO_XMA_SEEK_GUARD");
        if (!noGuard && d.lastBuffer >= 0 && d.holdsTail && !d.flush)
        {
            // The packets themselves decide when they can be read: the title restarts a context with another sound's
            // packets (often packet 1 of it) in the same input buffers at the same offsets, which no address or index shows.
            const uint8_t* thisPacket = GuestPtr<uint8_t>(buffer + packet * kPacketBytes);
            bool contiguous = (int(current) == d.lastBuffer && packet == d.lastPacket + 1) || (d.lastPacket + 1 >= d.lastCount && packet == 0);
            if (d.pendingBits >= 0)
                contiguous = NoFrameStart(thisPacket) ? d.pendingBits >= int(kPacketBits - 32) : unsigned(d.pendingBits) == FirstFrameOffset(thisPacket);
            else if (NoFrameStart(thisPacket))
                contiguous = false; // nothing to say the frame it carries began in the packets before
            if (!contiguous)
            {
                d.flush = true;
                d.guardFlushes++, g_guardFlushes++;
                if (g_traceXma && g_traceBudget.fetch_sub(1) > 0)
                    Log("xma: %08X discontinuity: buffer %u packet %u after buffer %d packet %u of %u (seeked %d): codec flushed", ctx, current, packet,
                        d.lastBuffer, d.lastPacket, d.lastCount, int(d.seeked));
            }
        }
        if (!d.Reset(ctx))
            return false;
        FedInfo info;
        info.ctx = ctx, info.bufferAddr = bufferAddr, info.packetIndex = packet, info.readOffset = Field(ctx, 2, 0, 26);
        info.bufferIndex = uint8_t(current), info.rateBits = uint8_t(rateBits), info.chBit = uint8_t(chBit);
        const uint8_t* fed = GuestPtr<uint8_t>(buffer + packet * kPacketBytes);
        if (!noGuard && NoFrameStart(fed) && !d.used)
        {
            // A new codec has no frame for this packet to continue, and would carry its "skip 32767 bits" into the next packet.
            d.pendingBits = -1;
        }
        else
        {
            const bool decoded = d.Decode(fed, info);
            if (decoded)
            {
                d.lastBuffer = int(current), d.lastPacket = packet, d.lastCount = packets;
                d.pendingBits = NoFrameStart(fed) ? (d.pendingBits >= int(kPacketBits - 32) ? d.pendingBits - int(kPacketBits - 32) : -1) : SpillBits(fed);
            }
            else
            {
                d.pendingBits = -1;
                if (g_traceXma && g_traceBudget.fetch_sub(1) > 0)
                    Log("xma: %08X decode error: buffer %u packet %u of %u", ctx, current, packet, packets);
            }
        }
    }

    // Loop: when the packet holding the loop end is done and loops remain, continue from
    // the loop start (packet-accurate).
    const uint32_t loops = Field(ctx, 0, 12, 8), loopEnd = Field(ctx, 4, 0, 26);
    if (loops && loopEnd / kPacketBits == packet)
    {
        if (g_traceXma)
            Log("xma: %08X loops back: packet %u, loop %u-%u, %u loops left", ctx, packet, Field(ctx, 3, 0, 26), loopEnd, loops);
        if (loops != 255)
            SetField(ctx, 0, 12, 8, loops - 1);
        SetField(ctx, 2, 0, 26, Field(ctx, 3, 0, 26));
        d.expectedOffset = Field(ctx, 2, 0, 26), d.haveExpected = true;
        d.flush = true;
        return true;
    }
    const uint32_t offset = (packet + 1) * kPacketBits + 32;
    if (packet + 1 >= packets)
    {
        d.consumed[current] = true; // (valid for the title until its audio is delivered)
        SetField(ctx, 4, 31, 1, current ^ 1);
        SetField(ctx, 2, 0, 26, 32);
    }
    else
        SetField(ctx, 2, 0, 26, offset);
    d.expectedOffset = Field(ctx, 2, 0, 26), d.haveExpected = true;
    return true;
}

// Once every 5 s while the counts changed: decode failures, guard flushes and packets decoded in that time.
void ReportStats()
{
    static auto last = Clock::now();
    static uint64_t errors = 0, flushes = 0, packets = 0, reopens = 0, waits = 0, errorPkts = 0, dumps = 0, bufs = 0, rates = 0, offs = 0, kicks = 0, locks = 0, clears = 0;
    const auto now = Clock::now();
    if (now - last < std::chrono::seconds(5))
        return;
    last = now;
    const uint64_t e = g_decodeErrors, f = g_guardFlushes, p = g_packetsDecoded, r = g_codecReopens, w = g_poolWaits, ep = g_errorPackets, dm = g_dumps, bc = g_bufferChanges,
                   rc = g_rateChanges, ow = g_offsetWrites, kk = g_regKicks, lk = g_regLocks, cl = g_regClears;
    if (e != errors || f != flushes || r != reopens || w != waits || ep != errorPkts || bc != bufs || rc != rates || ow != offs || cl != clears)
        Log("xma: 5 s: %llu decode errors, %llu discontinuity flushes, %llu codec reopens, %llu pool waits, %llu packets decoded; %llu packets FFmpeg complained about "
            "(%llu dumps so far); contexts: %llu buffer moves, %llu rate/channel changes, %llu offset writes by the title; registers: %llu kicks, %llu locks, %llu clears",
            (unsigned long long)(e - errors), (unsigned long long)(f - flushes), (unsigned long long)(r - reopens), (unsigned long long)(w - waits),
            (unsigned long long)(p - packets), (unsigned long long)(ep - errorPkts), (unsigned long long)dm, (unsigned long long)(bc - bufs),
            (unsigned long long)(rc - rates), (unsigned long long)(ow - offs), (unsigned long long)(kk - kicks), (unsigned long long)(lk - locks),
            (unsigned long long)(cl - clears));
    errors = e, flushes = f, packets = p, reopens = r, waits = w, errorPkts = ep, dumps = dm, bufs = bc, rates = rc, offs = ow, kicks = kk, locks = lk, clears = cl;
}

void DecoderLoop()
{
    SetThreadDescription(GetCurrentThread(), L"xma decoder");
    static LiveSwitch noBoost("FM2_NO_XMA_BOOST");
    if (!noBoost)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    auto next = Clock::now();
    for (;;)
    {
        next += std::chrono::milliseconds(2);
        std::this_thread::sleep_until(next);
        RefillCodecPool();
        std::lock_guard lock(g_mutex);
        for (uint32_t ctx : g_enabled)
            while (Step(ctx))
                ;
        ReportStats();
    }
}
}

namespace
{
// The context with a hardware ID (Kick/Lock/Clear bit).
uint32_t ContextById(uint32_t id)
{
    return g_contextArray + id * kContextSize;
}

// A context's state reset, as the Clear register and a re-used context do: no valid buffers, reading
// from the first packet's first frame, an empty output ring, and a fresh decoder.
void ResetDecoder(uint32_t ctx)
{
    if (auto it = g_decoders.find(ctx); it != g_decoders.end())
    {
        it->second.Close();
        g_decoders.erase(it);
    }
}

void ClearContext(uint32_t ctx)
{
    SetField(ctx, 0, 20, 2, 0);  // input buffers 0 and 1 not valid
    SetField(ctx, 1, 31, 1, 0);  // output buffer not valid
    SetField(ctx, 2, 0, 26, 32); // read offset: past the first packet's header
    SetField(ctx, 9, 0, 5, 0);   // output read offset
    SetField(ctx, 0, 27, 5, 0);  // output write offset
    ResetDecoder(ctx);
}

void StartDecoder()
{
    std::call_once(g_started, [] {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(XmaLogCallback);
        std::thread(DecoderLoop).detach();
    });
}
}

void XmaInit()
{
    g_contextArray = GuestAlloc(Heap::Physical, kContextCount * kContextSize, 0x100);
    memset(GuestPtr(g_contextArray), 0, kContextCount * kContextSize);
    SetRegister(kRegContextArrayAddress, g_contextArray & 0x1FFFFFFF);
    SetRegister(kRegNextContextIndex, 1);
}

// A store to an APU register (PPCMmioStore32): value as the register takes it.
void XmaWriteRegister(uint32_t address, uint32_t value)
{
    const uint32_t r = (address & 0xFFFF) / 4;
    std::lock_guard lock(g_mutex);
    SetRegister(r, value);
    auto forEach = [&](uint32_t group, auto&& fn) {
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value & (1u << bit))
                if (const uint32_t id = (r - group) * 32 + bit; id < kContextCount)
                    fn(ContextById(id));
    };
    if (r >= kRegKick && r < kRegKick + 10)
    {
        // Kick: start (or continue) decoding these contexts, as XMAEnableContext does.
        StartDecoder();
        forEach(kRegKick, [](uint32_t c) {
            g_regKicks++;
            g_enabled.insert(c);
            while (Step(c))
                ;
        });
    }
    else if (r >= kRegLock && r < kRegLock + 10)
    {
        // Lock: stop decoding them, as XMADisableContext does. Taking g_mutex above means no Step is
        // under way, so the title may edit them once this store returns.
        forEach(kRegLock, [](uint32_t c) { g_regLocks++, g_enabled.erase(c); });
    }
    else if (r >= kRegClear && r < kRegClear + 10)
        forEach(kRegClear, [](uint32_t c) {
            g_regClears++;
            if (g_idBudget.fetch_sub(1) > 0)
                Log("xma: id %08X: clear register", c);
            ClearContext(c);
        });
    else
        LOG_ONCE("xma: write %08X to APU register %03X", value, r);
}

GUEST_EXPORT(XMACreateContext)
{
    uint32_t context = 0;
    {
        std::lock_guard lock(g_mutex);
        for (uint32_t id = 0; id < kContextCount && !context; id++)
            if (!g_contextUsed[id])
            {
                g_contextUsed[id] = true;
                context = ContextById(id);
            }
    }
    StoreIfNonNull(ctx.r3.u32, context);
    StartDecoder();
    Return(ctx, context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY);
}

GUEST_EXPORT(XMAReleaseContext)
{
    const uint32_t c = ctx.r3.u32;
    std::lock_guard lock(g_mutex);
    if (c >= g_contextArray && c < g_contextArray + kContextCount * kContextSize)
    {
        g_enabled.erase(c);
        ResetDecoder(c);
        memset(GuestPtr(c), 0, kContextSize);
        g_contextUsed[(c - g_contextArray) / kContextSize] = false;
    }
    Return(ctx, 0);
}

GUEST_EXPORT(XMAInitializeContext)
{
    // XMA_CONTEXT_INIT: in0, in0 packets, in1, in1 packets, read offset,
    // output, output blocks, work buffer, subframes, channels, sample rate,
    // loop start, loop end, loop count/subframe end/skip (bytes).
    const uint32_t c = ctx.r3.u32, init = ctx.r4.u32;
    std::lock_guard lock(g_mutex);
    memset(GuestPtr(c), 0, kContextSize);
    auto in = [&](uint32_t off) { return LoadBE32(init + off); };
    StoreBE32(c + 5 * 4, ToContextBuffer(in(0x00)));
    SetField(c, 0, 0, 12, in(0x04));
    StoreBE32(c + 6 * 4, ToContextBuffer(in(0x08)));
    SetField(c, 1, 0, 12, in(0x0C));
    SetField(c, 2, 0, 26, in(0x10));
    StoreBE32(c + 7 * 4, ToContextBuffer(in(0x14)));
    SetField(c, 0, 22, 5, in(0x18));
    StoreBE32(c + 8 * 4, in(0x1C));
    SetField(c, 1, 20, 4, in(0x20));
    SetField(c, 1, 29, 1, in(0x24) >= 1);
    SetField(c, 1, 27, 2, in(0x28));
    SetField(c, 3, 0, 26, in(0x2C));
    SetField(c, 4, 0, 26, in(0x30));
    SetField(c, 0, 12, 8, *GuestPtr<uint8_t>(init + 0x34));
    if (g_traceXma)
        Log("xma: init %08X in0 %08X x%u in1 %08X x%u offset %u out %08X blocks %u ch %u rate %u loop %u-%u x%u", c, in(0x00), in(0x04), in(0x08), in(0x0C),
            in(0x10), in(0x14), in(0x18), in(0x24), in(0x28), in(0x2C), in(0x30), *GuestPtr<uint8_t>(init + 0x34));
    if (auto it = g_decoders.find(c); it != g_decoders.end())
    {
        if (g_traceXma)
            Log("xma: re-init %08X: %zu packets decoded, %zu frames decoded, %zu delivered, %zu discarded unread", c, it->second.packetsDecoded,
                it->second.framesDecoded, it->second.framesWritten, (it->second.pcm.size() - it->second.pcmRead) / std::max<uint32_t>(it->second.channels, 1));
        it->second.packetsDecoded = it->second.framesDecoded = it->second.framesWritten = 0;
        it->second.consumed[0] = it->second.consumed[1] = false;
        it->second.holdsTail = false;
        it->second.flush = true;
        it->second.pcm.clear();
        it->second.pcmRead = 0;
    }
    Return(ctx, 0);
}

GUEST_EXPORT(XMAEnableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: enable %08X (read offset %u, in0 %u in1 %u valid, current %u) loops %u start %u end %u, subframes: end %u skip %u decode %u, packets %u/%u",
            ctx.r3.u32, Field(ctx.r3.u32, 2, 0, 26), Field(ctx.r3.u32, 0, 20, 1), Field(ctx.r3.u32, 0, 21, 1), Field(ctx.r3.u32, 4, 31, 1),
            Field(ctx.r3.u32, 0, 12, 8), Field(ctx.r3.u32, 3, 0, 26), Field(ctx.r3.u32, 4, 0, 26), Field(ctx.r3.u32, 1, 14, 3), Field(ctx.r3.u32, 1, 17, 3),
            Field(ctx.r3.u32, 1, 20, 4), Field(ctx.r3.u32, 0, 0, 12), Field(ctx.r3.u32, 1, 0, 12));
    g_enabled.insert(ctx.r3.u32);
    while (Step(ctx.r3.u32))
        ;
    Return(ctx, 0);
}

GUEST_EXPORT(XMADisableContext)
{
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: disable %08X", ctx.r3.u32);
    g_enabled.erase(ctx.r3.u32);
    Return(ctx, 0);
}

#define XMA_FIELD_EXPORTS(Name, dword, lo, bits)                     \
    GUEST_EXPORT(XMAGet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, lo, bits));             \
    }                                                                \
    GUEST_EXPORT(XMASet##Name)                                       \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        SetField(ctx.r3.u32, dword, lo, bits, ctx.r4.u32);           \
        Return(ctx, 0);                                              \
    }

GUEST_EXPORT(XMAGetInputBufferReadOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 2, 0, 26));
}

GUEST_EXPORT(XMASetInputBufferReadOffset)
{
    // A seek: the decoder starts over at the new packet.
    std::lock_guard lock(g_mutex);
    if (g_traceXma)
        Log("xma: seek %08X to offset %u (was %u), in0 %08X x%u in1 %08X x%u, out %08X blocks %u write %u read %u, skip %u", ctx.r3.u32, ctx.r4.u32,
            Field(ctx.r3.u32, 2, 0, 26), LoadBE32(ctx.r3.u32 + 20), Field(ctx.r3.u32, 0, 0, 12), LoadBE32(ctx.r3.u32 + 24), Field(ctx.r3.u32, 1, 0, 12),
            LoadBE32(ctx.r3.u32 + 28), Field(ctx.r3.u32, 0, 22, 5), Field(ctx.r3.u32, 0, 27, 5), Field(ctx.r3.u32, 9, 0, 5), Field(ctx.r3.u32, 1, 17, 3));
    SetField(ctx.r3.u32, 2, 0, 26, ctx.r4.u32);
    // The title restarts the decoder at the first frame of each next pair of packets (a seek
    // within a stream it reads in order). The codec keeps its overlap and its unfinished frame
    // across that, as the hardware does; resetting it dropped most of every pair's audio.
    static LiveSwitch flushOnSeek("FM2_XMA_SEEK_FLUSH");
    if (auto it = g_decoders.find(ctx.r3.u32); it != g_decoders.end())
    {
        it->second.seeked = true;
        if (flushOnSeek)
            it->second.flush = true;
    }
    Return(ctx, 0);
}
XMA_FIELD_EXPORTS(OutputBufferReadOffset, 9, 0, 5)

GUEST_EXPORT(XMAGetOutputBufferWriteOffset)
{
    std::lock_guard lock(g_mutex);
    Return(ctx, Field(ctx.r3.u32, 0, 27, 5));
}

#define XMA_FLAG_EXPORTS(Name, dword, bit)                           \
    GUEST_EXPORT(XMAIs##Name##Valid)                                 \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        Return(ctx, Field(ctx.r3.u32, dword, bit, 1));               \
    }                                                                \
    GUEST_EXPORT(XMASet##Name##Valid)                                \
    {                                                                \
        std::lock_guard lock(g_mutex);                               \
        if (g_traceXma)                                                          Log("xma: %08X " #Name " valid", ctx.r3.u32);                    SetField(ctx.r3.u32, dword, bit, 1, 1);                              ClearConsumed(ctx.r3.u32, dword, bit);                       \
        Return(ctx, 0);                                              \
    }

XMA_FLAG_EXPORTS(InputBuffer0, 0, 20)
XMA_FLAG_EXPORTS(InputBuffer1, 0, 21)
XMA_FLAG_EXPORTS(OutputBuffer, 1, 31)

// A read from a music file (imports_io.cpp): its packets' hashes, located from the file's RIFF data chunk.
void AudioNoteMusicRead(const std::filesystem::path& file, uint64_t offset, const uint8_t* data, uint32_t bytes)
{
    static std::map<std::filesystem::path, uint64_t> dataStart;
    std::lock_guard lock(g_musicMutex);
    auto it = dataStart.find(file);
    if (it == dataStart.end())
    {
        uint64_t start = 0;
        if (FILE* f = _wfopen(file.c_str(), L"rb"))
        {
            uint8_t head[4096] = {};
            const size_t n = fread(head, 1, sizeof(head), f);
            fclose(f);
            for (size_t i = 12; i + 8 <= n; i++)
                if (!memcmp(head + i, "data", 4))
                {
                    start = i + 8;
                    break;
                }
        }
        it = dataStart.emplace(file, start).first;
    }
    if (!it->second)
        return;
    const uint64_t first = it->second + (offset > it->second ? (offset - it->second + kPacketBytes - 1) / kPacketBytes * kPacketBytes : 0);
    for (uint64_t p = first; p + 64 <= offset + bytes; p += kPacketBytes)
        g_musicPackets.insert(PacketHash(data + (p - offset)));
}
