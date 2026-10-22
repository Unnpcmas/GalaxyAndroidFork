// OS time base, alarms (the decrementer), and miscellaneous OS services.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <exception>
#include <stdlib.h>
#include <typeinfo>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#endif
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "port/port.h"
#include "revolution/os.h"
#include "revolution/os/OSError.h"
#include "revolution/os/OSReset.h"
#include "revolution/os/OSResetSW.h"

// ---------------------------------------------------------------------------
// Time base.  OSTime counts timer ticks (bus clock / 4 = 60.75 MHz) since
// 2000-01-01 00:00:00, like the console after the IPL loads the RTC.
// ---------------------------------------------------------------------------
static constexpr int64_t kTimerClock = 243000000 / 4;
static int64_t sBootNs;
static int64_t sBootTicks;

extern "C" int64_t port_host_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

extern "C" void port_host_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ll);
    ts.tv_nsec = (long)(ns % 1000000000ll);
    nanosleep(&ts, nullptr);
}

static inline int64_t nsToTicks(int64_t ns) { return (ns / 4000) * 243 + ((ns % 4000) * 243) / 4000; }
static inline int64_t ticksToNs(int64_t ticks) { return (ticks / 243) * 4000 + ((ticks % 243) * 4000) / 243; }

static void initTimeBase() {
    sBootNs = port_host_time_ns();
    // Seconds from 2000-01-01 to now, in local time (the Wii RTC is local).
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t localNow = (int64_t)now + lt.tm_gmtoff;
    const int64_t kEpoch2000 = 946684800;  // 2000-01-01 in Unix time
    sBootTicks = (localNow - kEpoch2000) * kTimerClock;
}

static inline OSTime rawTime() { return sBootTicks + nsToTicks(port_host_time_ns() - sBootNs); }

extern "C" {

OSTime OSGetTime(void) {
    port_irq_poll();
    return rawTime();
}

OSTick OSGetTick(void) {
    port_irq_poll();
    return (OSTick)rawTime();
}

OSTime __OSGetSystemTime(void) { return rawTime(); }
OSTime __OSTimeToSystemTime(OSTime t) { return t; }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    int64_t totalUs = ticks / 243 * 4 + (ticks % 243) * 4 / 243;  // 60.75 ticks per microsecond
    int64_t secs = totalUs / 1000000;
    int64_t usecs = totalUs % 1000000;
    if (usecs < 0) {
        usecs += 1000000;
        secs -= 1;
    }
    time_t t = (time_t)(secs + 946684800);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    td->sec = tmv.tm_sec;
    td->min = tmv.tm_min;
    td->hour = tmv.tm_hour;
    td->mday = tmv.tm_mday;
    td->mon = tmv.tm_mon;
    td->year = tmv.tm_year + 1900;
    td->wday = tmv.tm_wday;
    td->yday = tmv.tm_yday;
    td->msec = (int)(usecs / 1000);
    td->usec = (int)(usecs % 1000);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Alarms.  The queue is owned by the CPU; a host timer thread raises the
// alarm interrupt when the earliest deadline passes.
// ---------------------------------------------------------------------------
static OSAlarm* sAlarmHead;
static std::mutex sTimerMutex;
static std::condition_variable sTimerCv;
static int64_t sTimerDeadline = INT64_MAX;  // in OSTime ticks, guarded by sTimerMutex

static void rearmTimer() {
    std::lock_guard<std::mutex> lk(sTimerMutex);
    sTimerDeadline = sAlarmHead ? sAlarmHead->fire : INT64_MAX;
    sTimerCv.notify_all();
}

static void timerThread() {
    std::unique_lock<std::mutex> lk(sTimerMutex);
    for (;;) {
        if (sTimerDeadline == INT64_MAX) {
            sTimerCv.wait(lk);
            continue;
        }
        int64_t now = rawTime();
        if (now >= sTimerDeadline) {
            sTimerDeadline = INT64_MAX;  // re-armed by the handler
            lk.unlock();
            port_irq_raise(PORT_IRQ_ALARM);
            lk.lock();
            continue;
        }
        int64_t waitNs = ticksToNs(sTimerDeadline - now);
        sTimerCv.wait_for(lk, std::chrono::nanoseconds(waitNs));
    }
}

static void insertAlarm(OSAlarm* alarm) {
    OSAlarm* prev = nullptr;
    OSAlarm* next = sAlarmHead;
    while (next && next->fire <= alarm->fire) {
        prev = next;
        next = next->next;
    }
    alarm->prev = prev;
    alarm->next = next;
    if (next) {
        next->prev = alarm;
    }
    if (prev) {
        prev->next = alarm;
    } else {
        sAlarmHead = alarm;
    }
}

static void removeAlarm(OSAlarm* alarm) {
    if (alarm->next) {
        alarm->next->prev = alarm->prev;
    }
    if (alarm->prev) {
        alarm->prev->next = alarm->next;
    } else if (sAlarmHead == alarm) {
        sAlarmHead = alarm->next;
    }
    alarm->prev = alarm->next = nullptr;
}

static void alarmIrq() {
    OSTime now = rawTime();
    while (sAlarmHead && sAlarmHead->fire <= now) {
        OSAlarm* alarm = sAlarmHead;
        removeAlarm(alarm);
        OSAlarmHandler handler = alarm->handler;
        if (alarm->period) {
            alarm->fire += alarm->period;
            if (alarm->fire <= now) {
                // Skip missed periods rather than firing a burst.
                OSTime missed = (now - alarm->fire) / alarm->period + 1;
                alarm->fire += missed * alarm->period;
            }
            insertAlarm(alarm);
        } else {
            alarm->handler = nullptr;
        }
        if (handler) {
            handler(alarm, OSGetCurrentContext());
        }
    }
    rearmTimer();
}

extern "C" {

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
    alarm->prev = alarm->next = nullptr;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = 0;
    alarm->handler = handler;
    alarm->fire = rawTime() + tick;
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = 0;
    alarm->handler = handler;
    alarm->fire = time;
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = period;
    alarm->start = start;
    alarm->handler = handler;
    OSTime now = rawTime();
    if (period > 0 && start < now) {
        alarm->fire = start + period * ((now - start) / period + 1);
    } else {
        alarm->fire = start;
    }
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
        alarm->handler = nullptr;
        rearmTimer();
    }
    OSRestoreInterrupts(en);
}

void OSCancelAlarms(u32 tag) {
    BOOL en = OSDisableInterrupts();
    for (OSAlarm* a = sAlarmHead; a;) {
        OSAlarm* n = a->next;
        if (a->tag == tag) {
            removeAlarm(a);
            a->handler = nullptr;
        }
        a = n;
    }
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }
void OSSetAlarmUserData(OSAlarm* alarm, void* data) { alarm->userData = data; }
void* OSGetAlarmUserData(const OSAlarm* alarm) { return alarm->userData; }

}  // extern "C"

// ---------------------------------------------------------------------------
// Reporting / fatal errors
// ---------------------------------------------------------------------------
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "PetariVR"
#endif

// Set by the headless runner so logs also reach the adb shell.
extern "C" int port_log_to_stderr = 0;

// The app's own copy of its log (port_log_file): the system log keeps only a
// few minutes of a busy headset, too little to look into a session after it.
static std::mutex sLogFileLock;
static FILE* sLogFile = nullptr;

static long sLogFileBytes = 0;
// The in-memory diagnostics ring holds 5,000 complete entries. A 4 MiB
// session file keeps at least that much history while bounding local storage.
static const long kLogFileMax = 4 << 20;

static std::mutex sLogRingLock;
static constexpr int kMaxLogRing = PORT_LOG_RING_CAPACITY;
static constexpr int kMaxLogLineLen = PORT_LOG_LINE_LENGTH;
static char sLogRing[kMaxLogRing][kMaxLogLineLen];
static int sLogRingStart = 0;
static int sLogRingCount = 0;

static void appendLogRingLineUnlocked(const char* line) {
    int slot = (sLogRingStart + sLogRingCount) % kMaxLogRing;
    if (sLogRingCount >= kMaxLogRing) {
        slot = sLogRingStart;
        sLogRingStart = (sLogRingStart + 1) % kMaxLogRing;
    } else {
        sLogRingCount++;
    }
    snprintf(sLogRing[slot], kMaxLogLineLen, "%s", line ? line : "");
}


#if defined(__ANDROID__)
// ---------------------------------------------------------------------------
// Crash report.  A fatal signal (or an uncaught C++ exception) writes a detailed
// report to galaxy_log.txt AND to crash_report.txt next to it before the process
// dies: the reason in plain words, thread, registers, memory use, the native
// backtrace (module + offset) and the last log lines (what the game was doing).
// The next start copies the report into the new log under
// "----- previous crash report -----", so one exported log tells the whole story.
// ---------------------------------------------------------------------------
namespace {
char sCrashReportPath[512] = {};
char sTerminateMsg[256] = {};
std::atomic<int> sInCrash{0};

struct CrashOut { char* buf; int len; int cap; };
void cprintf(CrashOut* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void cprintf(CrashOut* o, const char* fmt, ...) {
    if (o->len >= o->cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->len, (size_t)(o->cap - o->len), fmt, ap);
    va_end(ap);
    if (w > 0) o->len = (o->len + w < o->cap) ? o->len + w : o->cap - 1;
}

struct CrashBt { CrashOut* out; int n; };
_Unwind_Reason_Code crashBtStep(struct _Unwind_Context* ctx, void* arg) {
    CrashBt* bt = (CrashBt*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (!pc || bt->n >= 28) return _URC_END_OF_STACK;
    Dl_info info;
    if (dladdr((void*)pc, &info) && info.dli_fname) {
        const char* base = strrchr(info.dli_fname, '/');
        base = base ? base + 1 : info.dli_fname;
        cprintf(bt->out, "  #%02d %s+0x%lx %s\n", bt->n, base, (unsigned long)(pc - (uintptr_t)info.dli_fbase),
                info.dli_sname ? info.dli_sname : "");
    } else {
        cprintf(bt->out, "  #%02d pc=0x%lx (unknown module)\n", bt->n, (unsigned long)pc);
    }
    bt->n++;
    return _URC_NO_REASON;
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    default: return "signal";
    }
}

void crashReason(CrashOut* o, int sig, siginfo_t* info) {
    const unsigned long addr = info ? (unsigned long)info->si_addr : 0;
    const int code = info ? info->si_code : 0;
    switch (sig) {
    case SIGSEGV:
        if (code == SEGV_MAPERR) {
            if (addr < 0x10000) {
                cprintf(o, "REASON: null pointer dereference (read/write at offset 0x%lx of a null object)\n", addr);
            } else {
                cprintf(o, "REASON: access to unmapped memory at 0x%lx (dangling/freed pointer, buffer overrun or "
                           "stale handle)\n", addr);
            }
        } else if (code == SEGV_ACCERR) {
            cprintf(o, "REASON: forbidden access at 0x%lx (write to read-only memory or execute of data)\n", addr);
        } else {
            cprintf(o, "REASON: invalid memory access at 0x%lx (si_code %d)\n", addr, code);
        }
        break;
    case SIGBUS:
        cprintf(o, "REASON: bus error at 0x%lx (%s)\n", addr,
                code == BUS_ADRALN ? "misaligned access" : code == BUS_ADRERR ? "address does not exist (file/mmap)"
                                                                               : "hardware memory fault");
        break;
    case SIGABRT:
        if (sTerminateMsg[0]) {
            cprintf(o, "REASON: %s\n", sTerminateMsg);
        } else {
            cprintf(o, "REASON: abort() called: failed assertion, heap corruption detected by the allocator "
                       "(double free / overflow) or a deliberate abort\n");
        }
        break;
    case SIGFPE:
        cprintf(o, "REASON: arithmetic error (%s)\n", code == FPE_INTDIV ? "integer divide by zero" : "floating point");
        break;
    case SIGILL:
        cprintf(o, "REASON: illegal instruction (corrupt code, jump through a bad function pointer)\n");
        break;
    default:
        cprintf(o, "REASON: fatal signal %d\n", sig);
    }
}

void crashMemory(CrashOut* o) {
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return;
    char tmp[2048];
    ssize_t n = read(fd, tmp, sizeof(tmp) - 1);
    close(fd);
    if (n <= 0) return;
    tmp[n] = '\0';
    const char* keys[] = {"VmSize:", "VmRSS:", "VmSwap:", "Threads:"};
    for (const char* key : keys) {
        const char* p = strstr(tmp, key);
        if (!p) continue;
        const char* e = strchr(p, '\n');
        cprintf(o, "  %.*s\n", e ? (int)(e - p) : 40, p);
    }
}

// A thread can arm a recovery point around code that may fault on bad guest data
// (the software audio mixer): a SIGSEGV/SIGBUS there jumps back instead of
// killing the game.  Counters tell the caller what happened.
// What the game was doing, kept current by the main loop and the scene code so a
// crash report can say it without asking anything of a possibly broken heap.
char sCrashScene[96] = "(no scene yet)";
char sCrashContext[1000] = "(no context yet)";
__thread sigjmp_buf* sCrashGuard = nullptr;
std::atomic<unsigned> sGuardFaults{0};
std::atomic<unsigned long> sGuardLastOffset{0};
std::atomic<unsigned long> sGuardLastAddr{0};

void crashHandler(int sig, siginfo_t* info, void* uctx) {
    if ((sig == SIGSEGV || sig == SIGBUS) && sCrashGuard) {
        unsigned long pcOff = 0;
#if defined(__aarch64__)
        if (uctx) {
            Dl_info di;
            const uintptr_t pc = (uintptr_t)((ucontext_t*)uctx)->uc_mcontext.pc;
            pcOff = dladdr((void*)pc, &di) ? (unsigned long)(pc - (uintptr_t)di.dli_fbase) : (unsigned long)pc;
        }
#endif
        sGuardLastOffset.store(pcOff);
        sGuardLastAddr.store(info ? (unsigned long)info->si_addr : 0);
        sGuardFaults.fetch_add(1);
        sigjmp_buf* g = sCrashGuard;
        sCrashGuard = nullptr;
        siglongjmp(*g, 1);
    }
    if (sInCrash.exchange(1) == 0) {
        static char buf[12288];
        CrashOut o{buf, 0, (int)sizeof(buf)};
        char thread[24] = {};
        prctl(PR_GET_NAME, (unsigned long)thread, 0, 0, 0);
        struct timespec up;
        clock_gettime(CLOCK_MONOTONIC, &up);
        cprintf(&o, "CRASH ==============================================\n");
        cprintf(&o, "CRASH: %s, si_code %d, thread \"%s\" (tid %ld), uptime %lds\n", signalName(sig),
                info ? info->si_code : 0, thread, (long)syscall(SYS_gettid), (long)up.tv_sec);
        crashReason(&o, sig, info);
#if defined(__aarch64__)
        if (uctx) {
            const mcontext_t& m = ((ucontext_t*)uctx)->uc_mcontext;
            cprintf(&o, "REGS: pc=0x%llx lr=0x%llx sp=0x%llx fault=0x%llx\n", (unsigned long long)m.pc,
                    (unsigned long long)m.regs[30], (unsigned long long)m.sp, (unsigned long long)m.fault_address);
            cprintf(&o, "REGS: x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x19=0x%llx x20=0x%llx\n",
                    (unsigned long long)m.regs[0], (unsigned long long)m.regs[1], (unsigned long long)m.regs[2],
                    (unsigned long long)m.regs[3], (unsigned long long)m.regs[19], (unsigned long long)m.regs[20]);
        }
#else
        (void)uctx;
#endif
        cprintf(&o, "SCENE: %.95s\n", sCrashScene);
        cprintf(&o, "CONTEXT: %.999s\n", sCrashContext);
        cprintf(&o, "MEMORY:\n");
        crashMemory(&o);
        cprintf(&o, "BACKTRACE (module+offset; resolve offsets with addr2line against the same build):\n");
        CrashBt bt{&o, 0};
        _Unwind_Backtrace(crashBtStep, &bt);
        // What the game was doing: the newest log lines (read without locking).
        cprintf(&o, "LAST LOG LINES (newest last):\n");
        const int keep = 28;
        const int total = sLogRingCount;
        for (int i = total > keep ? total - keep : 0; i < total; i++) {
            const char* line = sLogRing[(sLogRingStart + i) % kMaxLogRing];
            if (strstr(line, "  draw #") || strstr(line, "  marker:")) continue;  // render dump noise
            cprintf(&o, "  %.200s\n", line);
        }
        cprintf(&o, "CRASH END ============================================\n");
        if (sLogFile) {
            fflush(sLogFile);
            ssize_t ignored = write(fileno(sLogFile), buf, (size_t)o.len);
            (void)ignored;
            fsync(fileno(sLogFile));
        }
        if (sCrashReportPath[0]) {
            int fd = open(sCrashReportPath, O_WRONLY | O_CREAT | O_TRUNC, 0660);
            if (fd >= 0) {
                ssize_t ignored = write(fd, buf, (size_t)o.len);
                (void)ignored;
                fsync(fd);
                close(fd);
            }
        }
    }
    signal(sig, SIG_DFL);  // let Android produce its usual tombstone / terminate
    raise(sig);
}

void terminateHandler() {
    try {
        std::exception_ptr e = std::current_exception();
        if (e) std::rethrow_exception(e);
        snprintf(sTerminateMsg, sizeof(sTerminateMsg), "std::terminate() called with no active exception");
    } catch (const std::exception& ex) {
        snprintf(sTerminateMsg, sizeof(sTerminateMsg), "uncaught C++ exception: %s (%s)", ex.what(), typeid(ex).name());
    } catch (...) {
        snprintf(sTerminateMsg, sizeof(sTerminateMsg), "uncaught C++ exception of unknown type");
    }
    abort();
}

void installCrashHandler(const char* logPath) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    snprintf(sCrashReportPath, sizeof(sCrashReportPath), "%s", logPath);
    char* slash = strrchr(sCrashReportPath, '/');
    snprintf(slash ? slash + 1 : sCrashReportPath, sizeof(sCrashReportPath) - (slash ? (size_t)(slash + 1 - sCrashReportPath) : 0),
             "crash_report.txt");
    std::set_terminate(terminateHandler);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
}

// Copies a crash report left by the previous run into the log ring; true if one existed.
bool importPreviousCrashReport(const char* logPath) {
    char path[512];
    snprintf(path, sizeof(path), "%s", logPath);
    char* slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "crash_report.txt");
    FILE* f = fopen(path, "r");
    if (!f) return false;
    {
        std::lock_guard<std::mutex> ringLock(sLogRingLock);
        appendLogRingLineUnlocked("----- previous crash report -----");
        char line[kMaxLogLineLen];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) appendLogRingLineUnlocked(line);
        }
    }
    fclose(f);
    remove(path);
    return true;
}
}  // namespace

extern "C" void port_crash_set_scene(const char* scene, const char* stage) {
    snprintf(sCrashScene, sizeof(sCrashScene), "%s / %s", scene ? scene : "?", stage ? stage : "?");
}
extern "C" const char* port_crash_scene(void) { return sCrashScene; }
extern "C" void port_crash_set_context(const char* text) {
    if (!text) return;
    snprintf(sCrashContext, sizeof(sCrashContext), "%s", text);
}
extern "C" void port_crash_guard_set(void* jb) { sCrashGuard = (sigjmp_buf*)jb; }
extern "C" unsigned port_crash_guard_faults(void) { return sGuardFaults.load(); }
extern "C" unsigned long port_crash_guard_last_offset(void) { return sGuardLastOffset.load(); }
extern "C" unsigned long port_crash_guard_last_addr(void) { return sGuardLastAddr.load(); }
#else
extern "C" void port_crash_set_scene(const char*, const char*) {}
extern "C" void port_crash_set_context(const char*) {}
extern "C" const char* port_crash_scene(void) { return "(n/a)"; }
extern "C" void port_crash_guard_set(void*) {}
extern "C" unsigned port_crash_guard_faults(void) { return 0; }
extern "C" unsigned long port_crash_guard_last_offset(void) { return 0; }
extern "C" unsigned long port_crash_guard_last_addr(void) { return 0; }
#endif

extern "C" void port_log_file(const char* path) {
    // The previous session's log is kept beside it as <path>.prev.
    bool hadPreviousSession = false;
    bool previousSessionClean = false;
    char prev[512];
    snprintf(prev, sizeof(prev), "%s.prev", path);
    if (rename(path, prev) == 0) {
        FILE* previous = fopen(prev, "r");
        if (previous) {
            hadPreviousSession = true;
            if (fseek(previous, 0, SEEK_END) == 0) {
                long fileSize = ftell(previous);
                long tailBytes = (long)kMaxLogRing * kMaxLogLineLen;
                long start = (fileSize > tailBytes) ? fileSize - tailBytes : 0;
                if (start > 0) {
                    fseek(previous, start, SEEK_SET);
                    char discard[kMaxLogLineLen];
                    (void)fgets(discard, sizeof(discard), previous);
                } else {
                    rewind(previous);
                }

                std::lock_guard<std::mutex> ringLock(sLogRingLock);
                appendLogRingLineUnlocked("----- previous session log -----");
                char previousLine[kMaxLogLineLen];
                while (fgets(previousLine, sizeof(previousLine), previous)) {
                    previousLine[strcspn(previousLine, "\r\n")] = '\0';
                    if (strstr(previousLine, "APP_CMD_DESTROY") != nullptr) {
                        previousSessionClean = true;
                    }
                    if (previousLine[0]) appendLogRingLineUnlocked(previousLine);
                }
            }
            fclose(previous);
        }
    }
#if defined(__ANDROID__)
    const bool previousCrashed = importPreviousCrashReport(path);
#else
    const bool previousCrashed = false;
#endif
    {
        std::lock_guard<std::mutex> lock(sLogFileLock);
        sLogFile = fopen(path, "w");
        sLogFileBytes = 0;
        if (sLogFile) {
            setvbuf(sLogFile, nullptr, _IOLBF, 1024);  // each line reaches the file at once, crash or not
        }
#if defined(__ANDROID__)
        if (sLogFile) installCrashHandler(path);
#endif
    }
    if (previousCrashed) {
        port_log("ERROR previous run CRASHED: the full crash report is above, under \"previous crash report\"");
    } else if (hadPreviousSession && !previousSessionClean) {
        // Emit only after releasing sLogFileLock: port_log() takes that same
        // lock when it appends the warning to the new session file.
        port_log("WARN previous process log has no APP_CMD_DESTROY marker; it may have crashed, been force-stopped, or ended by Android (not proof of a crash)");
    }
}

extern "C" int port_get_recent_logs(char* out, int maxBytes) {
    if (!out || maxBytes <= 0) return 0;
    std::lock_guard<std::mutex> lock(sLogRingLock);
    out[0] = '\0';
    int written = 0;
    for (int i = 0; i < sLogRingCount; i++) {
        int idx = (sLogRingStart + i) % kMaxLogRing;
        int len = (int)strlen(sLogRing[idx]);
        if (written + len + 2 >= maxBytes) break;
        memcpy(out + written, sLogRing[idx], len);
        written += len;
        out[written++] = '\n';
        out[written] = '\0';
    }
    return written;
}

static void logToFile(const char* fmt, va_list args) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char lineBuf[kMaxLogLineLen];
    va_list ringCopy;
    va_copy(ringCopy, args);
    vsnprintf(lineBuf, sizeof(lineBuf), fmt, ringCopy);
    va_end(ringCopy);

    {
        std::lock_guard<std::mutex> lock(sLogRingLock);
        char timestamped[kMaxLogLineLen];
        snprintf(timestamped, sizeof(timestamped), "%02d:%02d:%02d.%03d %s",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000), lineBuf);
        appendLogRingLineUnlocked(timestamped);
    }

    std::lock_guard<std::mutex> lock(sLogFileLock);
    if (!sLogFile || sLogFileBytes > kLogFileMax) {
        return;
    }
    int n = fprintf(sLogFile, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    n += vfprintf(sLogFile, fmt, args);
    fputc('\n', sLogFile);
    sLogFileBytes += n > 0 ? n + 1 : 1;
}

static std::atomic<int> sLogEnabled{1};

extern "C" void port_log_set_enabled(int enabled) {
    sLogEnabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int port_log_is_enabled(void) {
    return sLogEnabled.load(std::memory_order_relaxed);
}

extern "C" void port_vlog(const char* fmt, va_list args) {
    // With the log turned off in settings only ERROR/FATAL lines are kept,
    // so a crash still has something to report.
    if (!sLogEnabled.load(std::memory_order_relaxed) &&
        strncmp(fmt, "ERROR", 5) != 0 && strncmp(fmt, "FATAL", 5) != 0) {
        return;
    }
    {
        va_list copy;
        va_copy(copy, args);
        logToFile(fmt, copy);
        va_end(copy);
    }
#ifdef __ANDROID__
    if (port_log_to_stderr) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
        va_end(copy);
    }
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, args);
#else
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
#endif
}

extern "C" void port_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    port_vlog(fmt, args);
    va_end(args);
}

extern "C" void port_fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char message[2048];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(message, sizeof(message), fmt, copy);
    va_end(copy);
    va_end(args);

    // Keep fatal failures in the in-app ring as well as Android's fatal log.
    port_log("FATAL: %s", message);
#ifdef __ANDROID__
    if (port_log_to_stderr) {
        fputs("FATAL: ", stderr);
        fputs(message, stderr);
        fputc('\n', stderr);
    }
    __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, "%s", message);
#endif
    abort();
}

extern "C" {

void OSVReport(const char* fmt, va_list args) {
    // Game messages are Shift-JIS; log them verbatim.
    char buf[1024];
    port_vsnprintf(buf, sizeof(buf), fmt, args);  // game format strings (MSL semantics)
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    if (n) {
        port_log("[OS] %s", buf);
    }
}

void OSReport(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    OSVReport(fmt, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    port_vsnprintf(buf, sizeof(buf), fmt, args);  // game format strings (MSL semantics)
    va_end(args);
    port_fatal("OSPanic %s:%d: %s", file, line, buf);
}

void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    port_fatal("OSFatal: %s", msg);
}

// Error handlers (exceptions never reach the game on the host).
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return nullptr;
}

void OSRegisterVersion(const char* version) { port_log("OSRegisterVersion: %s", version); }

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) { (void)info; }

OSPowerCallback OSSetPowerCallback(OSPowerCallback cb) {
    (void)cb;
    return nullptr;
}

BOOL OSGetResetButtonState(void) { return FALSE; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
void OSRestart(u32 code) { port_fatal("OSRestart(%u)", code); }
void OSRebootSystem(void) { port_fatal("OSRebootSystem"); }
void OSReturnToMenu(void) { port_fatal("OSReturnToMenu"); }
void OSShutdownSystem(void) { port_fatal("OSShutdownSystem"); }
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)forceMenu;
    port_fatal("OSResetSystem(%d, %u)", reset, resetCode);
}

void OSInit(void) {}

}  // extern "C"

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
extern "C" void port_os_time_init(void) {
    initTimeBase();
    port_irq_set_handler(PORT_IRQ_ALARM, alarmIrq);
    std::thread(timerThread).detach();
}
