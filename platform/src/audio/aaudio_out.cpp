// Host audio output: the game's AI DMA stream (16-bit stereo, right sample
// first, normally 32 kHz) played through AAudio.  Blocks go into a ring that
// AAudio's data callback drains at the device's pace; the AI clock
// (ai_dsp.cpp) keeps the ring topped up.  PETARI_WAV=<path> also records the
// stream to a WAV file.
#include <aaudio/AAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "port/port.h"
#include "revolution/types.h"

namespace {

std::mutex sLock;  // the stream, the resampler and the WAV file
AAudioStream* sStream = nullptr;
bool sOpenFailed = false;
std::atomic<bool> sDisconnected{false};
std::atomic<int> sLastStreamError{0};      // set by AAudio's error callback, logged from the producer thread
std::atomic<uint64_t> sTotalCallbackFrames{0};
int32_t sDeviceRate = 0;
std::vector<s16> sLR, sOut;

// Stereo resampling with 4-point cubic (Catmull-Rom) interpolation, across
// block boundaries.
struct Resampler {
    s16 hist[3][2] = {};  // the previous block's last 3 frames
    double pos = 1.0;     // next output position; index 0 = hist[0]

    void process(const s16* in, u32 frames, double step, std::vector<s16>& out) {
        auto at = [&](int k, int c) -> float { return k < 3 ? hist[k][c] : in[(k - 3) * 2 + c]; };
        int total = (int)frames + 3;
        for (;;) {
            int i = (int)pos;
            if (i + 2 >= total) break;
            float t = (float)(pos - i);
            for (int c = 0; c < 2; c++) {
                float p0 = at(i - 1, c), p1 = at(i, c), p2 = at(i + 1, c), p3 = at(i + 2, c);
                float v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
                out.push_back((s16)(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v));
            }
            pos += step;
        }
        s16 last[3][2];
        for (int k = 0; k < 3; k++) {
            last[k][0] = (s16)at(total - 3 + k, 0);
            last[k][1] = (s16)at(total - 3 + k, 1);
        }
        memcpy(hist, last, sizeof(hist));
        pos -= frames;
    }
};
Resampler sResampler;

// Interleaved L/R frames at the device rate: written by port_audio_submit,
// read by the data callback.  Positions count frames and wrap freely.
const u32 kRingFrames = 16384;  // 0.5 s at 32 kHz
s16 sRing[kRingFrames * 2];
std::atomic<u32> sRingWrite{0}, sRingRead{0};
std::atomic<int> sUnderruns{0};
std::atomic<u32> sMixPeak{0}, sSubmitPeak{0}, sCallbackPeak{0};
std::atomic<u32> sSubmittedFrames{0}, sCallbackFrames{0};
bool sPlaying = false;  // callback thread only: the ring had audio last time

FILE* sWav = nullptr;
u32 sWavFrames = 0;

u32 sampleMagnitude(s16 sample) {
    int value = (int)sample;
    return (u32)(value < 0 ? -value : value);
}

void recordPeak(std::atomic<u32>& peak, u32 magnitude) {
    u32 previous = peak.load(std::memory_order_relaxed);
    while (magnitude > previous &&
           !peak.compare_exchange_weak(previous, magnitude, std::memory_order_relaxed)) {
    }
}

// Output gain: silent while a cutscene is fast-forwarded (its sounds would
// come out in a jumble), with 30 ms ramps.
float sGain = 1.0f;

void applySkipGain(s16* lr, u32 frames, u32 rate) {
    float target = port_skip_fast_forwarding() ? 0.0f : 1.0f;
    if (sGain == target && target == 1.0f) {
        return;
    }
    float step = 1.0f / (rate * 0.03f);
    for (u32 i = 0; i < frames; i++) {
        sGain = target > sGain ? fminf(target, sGain + step) : fmaxf(target, sGain - step);
        lr[i * 2] = (s16)(lr[i * 2] * sGain);
        lr[i * 2 + 1] = (s16)(lr[i * 2 + 1] * sGain);
    }
}

// Wii remote speaker PCM (6 kHz mono), mixed into the output.
const u32 kSpeakerRate = 6000;
std::mutex sSpkLock;
s16 sSpk[4096];
u32 sSpkRead = 0, sSpkWrite = 0;  // ring positions (free-running)
double sSpkPhase = 0.0;

void mixSpeaker(s16* lr, u32 frames, u32 rate) {
    std::lock_guard<std::mutex> lock(sSpkLock);
    double step = (double)kSpeakerRate / (double)rate;
    for (u32 i = 0; i < frames; i++) {
        if (sSpkWrite - sSpkRead < 2) break;
        u32 a = sSpkRead & 4095, b = (sSpkRead + 1) & 4095;
        float s = sSpk[a] + (sSpk[b] - sSpk[a]) * (float)sSpkPhase;
        s *= 0.8f;  // speaker sounds sit a little under the main mix
        for (int c = 0; c < 2; c++) {
            int v = lr[i * 2 + c] + (int)s;
            lr[i * 2 + c] = (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        sSpkPhase += step;
        while (sSpkPhase >= 1.0) {
            sSpkPhase -= 1.0;
            sSpkRead++;
        }
    }
}

void writeWavHeader(FILE* f, u32 frames, u32 rate) {
    u32 dataBytes = frames * 4;
    u8 h[44];
    memcpy(h, "RIFF", 4);
    u32 v = 36 + dataBytes;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    u16 s = 1;
    memcpy(h + 20, &s, 2);  // PCM
    s = 2;
    memcpy(h + 22, &s, 2);  // stereo
    memcpy(h + 24, &rate, 4);
    v = rate * 4;
    memcpy(h + 28, &v, 4);
    s = 4;
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &dataBytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

// AAudio's real-time thread: copies queued frames out, silence when there
// are none.
aaudio_data_callback_result_t dataCallback(AAudioStream* stream, void*, void* audioData, int32_t numFrames) {
    s16* out = (s16*)audioData;
    u32 r = sRingRead.load(std::memory_order_relaxed);
    u32 avail = sRingWrite.load(std::memory_order_acquire) - r;
    u32 n = avail < (u32)numFrames ? avail : (u32)numFrames;
    u32 callbackPeak = 0;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (r + i) & (kRingFrames - 1);
        out[i * 2] = sRing[idx * 2];
        out[i * 2 + 1] = sRing[idx * 2 + 1];
        callbackPeak = std::max(callbackPeak, sampleMagnitude(out[i * 2]));
        callbackPeak = std::max(callbackPeak, sampleMagnitude(out[i * 2 + 1]));
    }
    recordPeak(sCallbackPeak, callbackPeak);
    sCallbackFrames.fetch_add(n, std::memory_order_relaxed);
    sTotalCallbackFrames.fetch_add(numFrames, std::memory_order_relaxed);
    if (n < (u32)numFrames) {
        memset(out + n * 2, 0, (size_t)(numFrames - n) * 4);
        if (sPlaying && !port_is_paused()) {
            sUnderruns++;
            if (stream) {
                int32_t cur = AAudioStream_getBufferSizeInFrames(stream);
                int32_t cap = AAudioStream_getBufferCapacityInFrames(stream);
                int32_t burst = AAudioStream_getFramesPerBurst(stream);
                if (burst > 0 && cur + burst <= cap) {
                    AAudioStream_setBufferSizeInFrames(stream, cur + burst);
                }
            }
        }
        sPlaying = false;
    } else {
        sPlaying = true;
    }
    sRingRead.store(r + n, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// The output device went away or changed: port_audio_submit reopens the
// stream (the callback may not do it itself).
void errorCallback(AAudioStream*, void*, aaudio_result_t error) {
    sLastStreamError.store((int)error);
    // Any stream error (a device switch, the screen recorder taking over the
    // audio route, a service restart) ends this stream: reopen it.
    sDisconnected = true;
}

bool openStream(u32 rate) {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    // The device's own rate (48 kHz on the Quest): asking for the game's
    // 32 kHz moves AAudio off its fast path, to 20 ms bursts.
    (void)rate;
    // Normal (non-MMAP) shared mode: screen recorders and Android's playback
    // capture cannot record the low-latency MMAP path, which left recordings
    // silent or cut the game's sound when recording started.  The game mixes a
    // whole frame at a time, so the extra ~10-20 ms is not noticeable.
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 29
    AAudioStreamBuilder_setAllowedCapturePolicy(b, AAUDIO_ALLOW_CAPTURE_BY_ALL);
#endif
    AAudioStreamBuilder_setDataCallback(b, dataCallback, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, errorCallback, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &sStream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        port_log("audio: AAudio open (normal shared mode) failed (%s), retrying with default mode...", AAudio_convertResultToText(r));
        if (AAudio_createStreamBuilder(&b) == AAUDIO_OK) {
            AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setChannelCount(b, 2);
            AAudioStreamBuilder_setSampleRate(b, 48000);
            AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
            AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
            AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
            AAudioStreamBuilder_setDataCallback(b, dataCallback, nullptr);
            AAudioStreamBuilder_setErrorCallback(b, errorCallback, nullptr);
            r = AAudioStreamBuilder_openStream(b, &sStream);
            AAudioStreamBuilder_delete(b);
        }
    }
    if (r != AAUDIO_OK) {
        port_log("audio: AAudio open failed: %s", AAudio_convertResultToText(r));
        sStream = nullptr;
        return false;
    }
    sDeviceRate = AAudioStream_getSampleRate(sStream);
    if (sDeviceRate <= 0) sDeviceRate = 48000;
    // Dynamic low-latency buffer sizing: start at 3 bursts (~6-12 ms latency),
    // and let dataCallback dynamically expand if hardware underruns occur.
    int32_t burst = AAudioStream_getFramesPerBurst(sStream);
    if (burst <= 0) burst = 192;
    int32_t cap = AAudioStream_getBufferCapacityInFrames(sStream);
    int32_t want = std::max(burst * 3, 960);  // at least 20 ms at 48 kHz
    int32_t desired = (cap > 0) ? std::min(want, cap) : want;
    AAudioStream_setBufferSizeInFrames(sStream, desired);
    sRingRead.store(sRingWrite.load());
    sResampler = Resampler();
    r = AAudioStream_requestStart(sStream);
    if (r != AAUDIO_OK) {
        port_log("audio: AAudio requestStart failed: %s", AAudio_convertResultToText(r));
        AAudioStream_close(sStream);
        sStream = nullptr;
        return false;
    }
    port_log("audio: output route device=%d session=%d usage=%d sharing=%d performance=%d channels=%d format=%d",
             AAudioStream_getDeviceId(sStream), (int)AAudioStream_getSessionId(sStream),
             (int)AAudioStream_getUsage(sStream), (int)AAudioStream_getSharingMode(sStream),
             (int)AAudioStream_getPerformanceMode(sStream), AAudioStream_getChannelCount(sStream),
             (int)AAudioStream_getFormat(sStream));
    port_log("audio: AAudio started: %d Hz (source %u Hz), burst %d frames, buffer %d frames", sDeviceRate, rate, burst, AAudioStream_getBufferSizeInFrames(sStream));
    return true;
}

}  // namespace

extern "C" void port_audio_submit(const s16* rightLeft, u32 frames, u32 rate) {
    std::lock_guard<std::mutex> lock(sLock);
    sLR.resize(frames * 2);
    for (u32 i = 0; i < frames; i++) {
        sLR[i * 2] = rightLeft[i * 2 + 1];
        sLR[i * 2 + 1] = rightLeft[i * 2];
    }
    mixSpeaker(sLR.data(), frames, rate);
    u32 mixPeak = 0;
    for (u32 i = 0; i < frames * 2; i++) {
        mixPeak = std::max(mixPeak, sampleMagnitude(sLR[i]));
    }
    recordPeak(sMixPeak, mixPeak);
    applySkipGain(sLR.data(), frames, rate);
    u32 submitPeak = 0;
    for (u32 i = 0; i < frames * 2; i++) {
        submitPeak = std::max(submitPeak, sampleMagnitude(sLR[i]));
    }
    recordPeak(sSubmitPeak, submitPeak);

    static bool wavChecked = false;
    if (!wavChecked) {
        wavChecked = true;
        if (const char* path = getenv("PETARI_WAV")) {
            if (*path && (sWav = fopen(path, "wb")) != nullptr) {
                writeWavHeader(sWav, 0, rate);
            }
        }
    }
    if (sWav) {
        fwrite(sLR.data(), 4, frames, sWav);
        sWavFrames += frames;
        if ((sWavFrames & 0x3FFF) < frames) {
            writeWavHeader(sWav, sWavFrames, rate);
            fflush(sWav);
        }
    }

    // Watchdog, once a second: the recorder or another app can pause, stop or
    // stall the stream without a clean error.  Report it and recover.
    {
        static int64_t nextWatchNs = 0;
        static uint64_t lastCallbackTotal = 0;
        static int64_t lastProgressNs = 0;
        const int64_t nowNs = port_host_time_ns();
        if (const int err = sLastStreamError.exchange(0)) {
            port_log("audio: stream error %d (%s); the output will be reopened", err, AAudio_convertResultToText(err));
        }
        if (sStream && nowNs >= nextWatchNs) {
            nextWatchNs = nowNs + 1000000000ll;
            const aaudio_stream_state_t st = AAudioStream_getState(sStream);
            const uint64_t total = sTotalCallbackFrames.load(std::memory_order_relaxed);
            if (total != lastCallbackTotal || lastProgressNs == 0) {
                lastCallbackTotal = total;
                lastProgressNs = nowNs;
            }
            if (!port_is_paused()) {
                if (st == AAUDIO_STREAM_STATE_PAUSED || st == AAUDIO_STREAM_STATE_STOPPED ||
                    st == AAUDIO_STREAM_STATE_FLUSHED) {
                    port_log("audio: stream was %d (not started) while the game runs; starting it again", (int)st);
                    AAudioStream_requestStart(sStream);
                } else if (st == AAUDIO_STREAM_STATE_DISCONNECTED || st == AAUDIO_STREAM_STATE_CLOSED ||
                           st == AAUDIO_STREAM_STATE_UNINITIALIZED) {
                    port_log("audio: stream state %d; reopening the output", (int)st);
                    sDisconnected.store(true);
                } else if (st == AAUDIO_STREAM_STATE_STARTED && nowNs - lastProgressNs > 2500000000ll) {
                    port_log("audio: the device has not read audio for %.1f s (state started); reopening the output",
                             (double)(nowNs - lastProgressNs) / 1e9);
                    sDisconnected.store(true);
                    lastProgressNs = nowNs;
                }
            } else {
                lastProgressNs = nowNs;
            }
        }
    }

    const bool reopenRequested = sDisconnected.exchange(false);
    if (reopenRequested) {
        if (sStream) {
            port_log("audio: closing stale output stream before reopen");
            AAudioStream_close(sStream);
            sStream = nullptr;
        } else {
            port_log("audio: no active output stream; retrying open");
        }
    }
    static int sRetryCounter = 0;
    if (!sStream) {
        if (sOpenFailed) {
            if (++sRetryCounter < 100) return; // Retry audio stream initialization after ~2 seconds
            sRetryCounter = 0;
            sOpenFailed = false;
        }
        sOpenFailed = !openStream(rate);
    }
    if (!sStream) return;

    const s16* out = sLR.data();
    u32 outFrames = frames;
    if ((u32)sDeviceRate != rate) {
        sOut.clear();
        sResampler.process(sLR.data(), frames, (double)rate / (double)sDeviceRate, sOut);
        out = sOut.data();
        outFrames = (u32)(sOut.size() / 2);
    }

    u32 w = sRingWrite.load(std::memory_order_relaxed);
    u32 space = kRingFrames - (w - sRingRead.load(std::memory_order_acquire));
    if (outFrames > space) {
        outFrames = space;  // the device stopped reading; the AI clock waits for it
    }
    for (u32 i = 0; i < outFrames; i++) {
        u32 idx = (w + i) & (kRingFrames - 1);
        sRing[idx * 2] = out[i * 2];
        sRing[idx * 2 + 1] = out[i * 2 + 1];
    }
    sRingWrite.store(w + outFrames, std::memory_order_release);
    sSubmittedFrames.fetch_add(outFrames, std::memory_order_relaxed);

    // A periodic breadcrumb distinguishes a silent game mix from a silent
    // Android output stream without doing logging on AAudio's real-time thread.
    static int64_t nextStatsAt = 0;
    int64_t now = port_host_time_ns();
    if (now >= nextStatsAt) {
        nextStatsAt = now + 5000000000ll;
        int state = sStream ? (int)AAudioStream_getState(sStream) : -1;
        port_log("audio: mix peak %u, output peak %u, callback peak %u; submitted %u, played %u frames, underruns %d, stream state %d",
                 sMixPeak.exchange(0), sSubmitPeak.exchange(0), sCallbackPeak.exchange(0),
                 sSubmittedFrames.exchange(0), sCallbackFrames.exchange(0), sUnderruns.load(), state);
    }
}

extern "C" void port_speaker_push(const s16* samples, int count) {
    std::lock_guard<std::mutex> lock(sSpkLock);
    for (int i = 0; i < count; i++) {
        if (sSpkWrite - sSpkRead >= 4096) sSpkRead++;  // drop the oldest when the output stalls
        sSpk[sSpkWrite & 4095] = samples[i];
        sSpkWrite++;
    }
}

// Times the device found the ring empty while the game was running.
extern "C" int port_audio_underruns(void) { return sUnderruns.load(); }

// Audio queued for the device, or -1 without one (the AI then runs on a
// timer).
extern "C" int64_t port_audio_queued_ns(void) {
    std::lock_guard<std::mutex> lock(sLock);
    if (!sStream || sDeviceRate <= 0) return sOpenFailed ? -1 : 0;
    u32 queued = sRingWrite.load(std::memory_order_relaxed) - sRingRead.load(std::memory_order_acquire);
    return (int64_t)queued * 1000000000ll / sDeviceRate;
}

extern "C" void port_audio_pause(void) {
    std::lock_guard<std::mutex> lock(sLock);
    if (sStream) {
        aaudio_result_t result = AAudioStream_requestPause(sStream);
        if (result != AAUDIO_OK) {
            port_log("audio: pause request failed (%s); output will reopen on resume",
                     AAudio_convertResultToText(result));
            sDisconnected.store(true);
        }
    }
}

extern "C" void port_audio_resume(void) {
    std::lock_guard<std::mutex> lock(sLock);
    // Close the paused stream here, on the lifecycle thread, rather than from
    // the AI producer thread. Its old ring depth must not keep the producer
    // waiting for a callback that cannot run while the stream is paused.
    if (sStream) {
        AAudioStream* pausedStream = sStream;
        sStream = nullptr;
        AAudioStream_close(pausedStream);
        port_log("audio: closed paused output stream during resume");
    }
    sRingRead.store(sRingWrite.load(std::memory_order_acquire), std::memory_order_release);
    sResampler = Resampler();
    sOpenFailed = false;
    sDisconnected.store(false, std::memory_order_release);
    port_log("audio: output queue reset on resume; waiting for next game audio block");
}
