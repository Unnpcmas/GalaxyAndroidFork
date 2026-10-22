// Flat-screen Android Mobile application layer (AOSP / Phones).
// Replaces the OpenXR bootstrap with ANativeWindow + EGL surface rendering
// and standard monoscopic presentation.

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gx/gl_renderer.h"
#include "gx/vk_renderer.h"
#include "gx/vk_shadergen.h"
#include "dvd/mod_manager.h"
#include "mobile_ui.h"
#include "freecam.h"
#include "port/compat.h"
#include "port/input.h"
#include "port/port.h"
#include "port/randomizer.h"
#include <android/sensor.h>

#define LOGI(...) port_log("INFO PetariVR: " __VA_ARGS__)
#define LOGW(...) port_log("WARN PetariVR: " __VA_ARGS__)
#define LOGE(...) port_log("ERROR PetariVR: " __VA_ARGS__)

extern "C" void port_mem_set_reserved_window(uintptr_t base, size_t size);
extern "C" void port_boot(const char* dataRoot, const char* saveRoot);
extern "C" void port_audio_pause(void);
extern "C" void port_audio_resume(void);
extern std::atomic<uint64_t> gPresentedFrames;

// ---- Freeze watchdog (read-only diagnostics) --------------------------------
// If no frame is presented for several seconds while the app is in the foreground,
// log what every thread is doing: a thread parked in a futex wait points to a
// deadlock, a thread burning CPU in state R points to an endless loop.
static std::atomic<bool> sAppPaused{false};

struct WatchThread {
    int tid;
    char comm[20];
    char state;
    unsigned long cpu;
};

static int watchSnapThreads(WatchThread* out, int maxThreads) {
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0;
    int n = 0;
    while (dirent* e = readdir(d)) {
        const int tid = atoi(e->d_name);
        if (tid <= 0 || n >= maxThreads) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char buf[512];
        const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        char* open = strchr(buf, '(');
        char* close = strrchr(buf, ')');
        if (!open || !close || close < open) continue;
        WatchThread& t = out[n];
        t.tid = tid;
        size_t cl = (size_t)(close - open - 1);
        if (cl > sizeof(t.comm) - 1) cl = sizeof(t.comm) - 1;
        memcpy(t.comm, open + 1, cl);
        t.comm[cl] = 0;
        char state = '?';
        unsigned long ut = 0, st = 0;
        if (sscanf(close + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) == 3) {
            t.state = state;
            t.cpu = ut + st;
            n++;
        }
    }
    closedir(d);
    return n;
}

static void watchReadProc(int tid, const char* file, char* out, size_t outSize) {
    out[0] = 0;
    char path[80];
    snprintf(path, sizeof(path), "/proc/self/task/%d/%s", tid, file);
    FILE* f = fopen(path, "r");
    if (!f) return;
    const size_t len = fread(out, 1, outSize - 1, f);
    fclose(f);
    out[len] = 0;
    for (size_t i = 0; i < len; i++) {
        if (out[i] == '\n') out[i] = 0;
    }
}

extern "C" void port_debug_dump_threads(void);
extern "C" const char* port_crash_scene(void);

static void watchReport(int stalledSec, uint64_t frames) {
    static WatchThread a[160];
    static WatchThread b[160];
    const int na = watchSnapThreads(a, 160);
    usleep(1000 * 1000);
    const int nb = watchSnapThreads(b, 160);
    port_log("ERROR watchdog: no frame presented for %d s (frames=%llu, scene=%s, %d threads)", stalledSec,
             (unsigned long long)frames, port_crash_scene(), nb);
    for (int i = 0; i < nb; i++) {
        unsigned long before = b[i].cpu;
        for (int j = 0; j < na; j++) {
            if (a[j].tid == b[i].tid) {
                before = a[j].cpu;
                break;
            }
        }
        char sys[96];
        char wch[48];
        watchReadProc(b[i].tid, "syscall", sys, sizeof(sys));
        watchReadProc(b[i].tid, "wchan", wch, sizeof(wch));
        port_log("watchdog: thread %d '%s' state=%c cpu+%lu ticks/s syscall=[%s] wchan=[%s]", b[i].tid, b[i].comm,
                 b[i].state, b[i].cpu - before, sys, wch);
    }
    // What every emulated game thread is waiting on, with its native backtrace.
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        port_debug_dump_threads();
    }
}

static void freezeWatchdogMain() {
    uint64_t last = gPresentedFrames.load();
    int stalled = 0;
    int reports = 0;
    for (;;) {
        sleep(1);
        if (sAppPaused.load()) {
            stalled = 0;
            reports = 0;
            last = gPresentedFrames.load();
            continue;
        }
        const uint64_t now = gPresentedFrames.load();
        if (now != last) {
            if (reports > 0) port_log("watchdog: frames flowing again after %d s", stalled);
            last = now;
            stalled = 0;
            reports = 0;
            continue;
        }
        stalled++;
        if (reports < 3 && (stalled == 8 || stalled == 30 || stalled == 90)) {
            reports++;
            watchReport(stalled, now);
        }
    }
}

static void startFreezeWatchdog() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread(freezeWatchdogMain).detach();
}

namespace {

struct MobileApp {
    android_app* android = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;
    GLuint fxaaProgram = 0;
    GLuint fxaaVao = 0;
    GLint fxaaImageLocation = -1;
    GLint fxaaTexelLocation = -1;
    GLint fxaaLevelLocation = -1;
    int fxaaInitState = 0; // 0 = not tried, 1 = ready, -1 = unavailable for this EGL context

    bool windowReady = false;
    bool eglReady = false;
    int screenWidth = 0;
    int screenHeight = 0;

    gpu::EfbTarget efbTarget;
    bool targetReady = false;
    // Reuse the last fully rendered game frame during brief periods with no
    // prepared frame, rather than clearing the visible game layer to black.
    bool lastCompleteFrameValid = false;
    bool loggedFrameHold = false;

    gpu::VkTarget vkTarget{};
    bool vkReady = false;
    int requestedRenderScale = -1;
    int requestedTargetWidth = 0;
    int requestedTargetHeight = 0;
    int activeRenderScale = -1;

    bool gameBooted = false;
    std::string dataRoot;
    std::string saveRoot;

    // Viewport calculation for 16:9 pillarbox/letterbox
    int vpX = 0, vpY = 0, vpW = 0, vpH = 0;

    // Input state
    PortPadState pad{};
    bool touchActive = false;
    float touchX = 0.0f, touchY = 0.0f;

    // Virtual touch controls state
    int stickPointerId = -1;
    float stickOriginX = 0.0f;
    float stickOriginY = 0.0f;
    int spinFrames = 0;
    bool gamepadDetected = false;
    int64_t nextControllerPollNs = 0;
    // Digital buttons and analog-trigger/hat buttons are tracked separately: a
    // pad streams axis updates continuously, and clearing the shared mask there
    // used to drop a button a key event had just pressed (and vice versa).
    uint32_t physicalButtons = 0;
    uint32_t physicalAxisButtons = 0;
    uint32_t physicalMotionButtons = 0;
    bool touchscreenDown = false;
    float physicalStickX = 0.0f;
    float physicalStickY = 0.0f;
    int64_t pointerNavLastNs = 0;
    bool pointerNavLogged = false;
};

MobileApp gMobile;

static uint32_t combinedPadButtons(const MobileApp& app) {
    uint32_t buttons = app.physicalButtons | app.physicalAxisButtons |
                       app.physicalMotionButtons |
                       mobile_ui::get().getTouchButtons();
    // The native title prompt asks for A+B, while touch confirmation already
    // supplies the paired input. Match that behavior for physical controllers
    // too; this is limited to the title/logo screens.
    if (port_is_title_screen() && (app.physicalButtons & 0x0800u)) {
        buttons |= 0x0400u;
        static std::atomic<bool> titleAssistLogged{false};
        if (!titleAssistLogged.exchange(true)) {
            LOGI("controller: title-screen A paired with B for the A+B prompt");
        }
    }
    return buttons;
}

struct RenderTargetSpec {
    int width;
    int height;
};

static int effectiveRenderScale(const mobile_ui::Settings& settings) {
    const int selected = std::max(0, std::min(4, settings.renderScale));
    // Battery saver is deliberately temporary: the user's selected quality is
    // retained and restored when saver mode is turned off.
    return settings.batterySaver ? std::min(selected, 1) : selected;
}

static RenderTargetSpec renderTargetSpec(int screenWidth, int screenHeight,
                                         bool fullscreen, int scale) {
    const int shortEdge = std::max(1, std::min(screenWidth, screenHeight));
    const int longEdge = std::max(1, std::max(screenWidth, screenHeight));
    const float aspect = (float)longEdge / (float)shortEdge;
    const int clampedScale = std::max(0, std::min(4, scale));

    if (clampedScale == 4) {
        // 3840 pixels on the long edge. On 16:9 this is 3840x2160; on wider
        // phone panels it preserves the panel aspect without rendering extra
        // pixels beyond the advertised 4K long edge.
        if (!fullscreen) return {3840, 2160};
        return {3840, std::max(1, (int)lroundf(3840.0f / aspect))};
    }

    static constexpr int kTargetHeights[] = {480, 720, 1080, 1440};
    const int height = kTargetHeights[clampedScale];
    const int width = fullscreen
        ? (int)lroundf((float)height * aspect)
        : (int)lroundf((float)height * (16.0f / 9.0f));
    return {std::max(1, width), height};
}

static bool targetRequestChanged(const MobileApp& app, int scale,
                                const RenderTargetSpec& requested) {
    return app.requestedRenderScale != scale ||
           app.requestedTargetWidth != requested.width ||
           app.requestedTargetHeight != requested.height;
}

static void rememberTargetRequest(MobileApp& app, int scale,
                                  const RenderTargetSpec& requested) {
    app.requestedRenderScale = scale;
    app.requestedTargetWidth = requested.width;
    app.requestedTargetHeight = requested.height;
}

template <typename Target, typename CreateTarget, typename IsValid>
static Target createTargetWithFallback(int screenWidth, int screenHeight,
                                       bool fullscreen, int requestedScale,
                                       const char* backend, int& activeScale,
                                       int& activeWidth, int& activeHeight,
                                       CreateTarget&& createTarget,
                                       IsValid&& isValid) {
    for (int scale = requestedScale; scale >= 0; --scale) {
        const RenderTargetSpec spec =
            renderTargetSpec(screenWidth, screenHeight, fullscreen, scale);
        Target target = createTarget(spec.width, spec.height);
        if (isValid(target)) {
            activeScale = scale;
            activeWidth = spec.width;
            activeHeight = spec.height;
            if (scale < requestedScale) {
                LOGW("%s could not allocate requested quality %d (%dx%d); using quality %d (%dx%d)",
                     backend, requestedScale, renderTargetSpec(screenWidth, screenHeight,
                     fullscreen, requestedScale).width,
                     renderTargetSpec(screenWidth, screenHeight, fullscreen,
                     requestedScale).height, scale, spec.width, spec.height);
            }
            return target;
        }
        LOGW("%s render target allocation failed: quality=%d size=%dx%d",
             backend, scale, spec.width, spec.height);
    }
    activeScale = -1;
    activeWidth = 0;
    activeHeight = 0;
    return Target{};
}

static ASensorManager* sSensorManager = nullptr;
static const ASensor* sGyroSensor = nullptr;
static const ASensor* sAccelSensor = nullptr;
static ASensorEventQueue* sSensorQueue = nullptr;
static bool sSensorsActive = false;

static void updateSensorsState(bool enable) {
    if (!sSensorQueue) return;
    if (enable && !sSensorsActive) {
        if (sGyroSensor) {
            ASensorEventQueue_enableSensor(sSensorQueue, sGyroSensor);
            ASensorEventQueue_setEventRate(sSensorQueue, sGyroSensor, 16666);
            LOGI("Sensors: Activated gyroscope");
        }
        if (sAccelSensor) {
            ASensorEventQueue_enableSensor(sSensorQueue, sAccelSensor);
            ASensorEventQueue_setEventRate(sSensorQueue, sAccelSensor, 16666);
            LOGI("Sensors: Activated accelerometer");
        }
        sSensorsActive = true;
    } else if (!enable && sSensorsActive) {
        if (sGyroSensor) {
            ASensorEventQueue_disableSensor(sSensorQueue, sGyroSensor);
        }
        if (sAccelSensor) {
            ASensorEventQueue_disableSensor(sSensorQueue, sAccelSensor);
        }
        sSensorsActive = false;
        LOGI("Sensors: Deactivated sensors");
    }
}

static void initSensors(android_app* app) {
    if (!sSensorManager) {
        sSensorManager = ASensorManager_getInstanceForPackage(nullptr);
        if (!sSensorManager) {
            sSensorManager = ASensorManager_getInstance();
        }
    }
    if (sSensorManager && !sSensorQueue) {
        sGyroSensor = ASensorManager_getDefaultSensor(sSensorManager, ASENSOR_TYPE_GYROSCOPE);
        sAccelSensor = ASensorManager_getDefaultSensor(sSensorManager, ASENSOR_TYPE_ACCELEROMETER);
        sSensorQueue = ASensorManager_createEventQueue(sSensorManager, app->looper, 3, nullptr, nullptr);
        sSensorsActive = false;
    }
}

// Phone-motion camera (Wii style).  The game's camera orbit is a rate control
// (stick 1.0 = 1.2 rad/s of yaw, 0.9 rad/s of pitch, times the camera
// sensitivity), so the phone's own angular velocity maps 1:1: turn the phone
// 90 degrees and the camera turns 90 degrees.  The gyro is integrated between
// frames (no sample aliasing), its bias is tracked while the phone rests, and
// the result is pre-distorted to undo the game's own stick shaping.
static double sGyroAccYaw = 0.0, sGyroAccPitch = 0.0, sGyroAccDt = 0.0;  // rad, rad, s since the last frame
static int64_t sGyroLastEventNs = 0;
static float sGyroBiasX = 0.0f, sGyroBiasY = 0.0f;
static float sLandscapeSign = 1.0f;     // +1 landscape (top to the left), -1 reverse landscape
static float sGyroStickX = 0.0f, sGyroStickY = 0.0f;
static int64_t sGyroCamNs = 0;
static bool sCamInjected = false;  // we are currently driving pad.rightStick

static float gyroToStick(float rate, float rateAtFullStick) {
    // rate (rad/s the camera should turn) -> stick value the game turns into that rate.
    const float t = std::min(1.0f, fabsf(rate) / rateAtFullStick);
    if (t <= 0.0f) return 0.0f;
    const float dz = 0.04f;                      // the game's own dead zone
    const float v = dz + (1.0f - dz) * powf(t, 1.25f);  // inverse of its pow(., 0.8) shaping
    return rate < 0 ? -v : v;
}

static void pollSensors(MobileApp& a) {
    if (!sSensorQueue) return;
    if (!sSensorsActive) {
        updateSensorsState(true);
    }

    ASensorEvent event;
    while (ASensorEventQueue_getEvents(sSensorQueue, &event, 1) > 0) {
        if (!sSensorsActive) continue;
        if (event.type == ASENSOR_TYPE_GYROSCOPE) {
            {
                const auto& cs = mobile_ui::UIOverlay::get().settings();
                if (cs.gyroCamera) {
                    const int64_t ts = event.timestamp;
                    double dt = sGyroLastEventNs ? (double)(ts - sGyroLastEventNs) * 1e-9 : 0.0;
                    sGyroLastEventNs = ts;
                    if (dt <= 0.0 || dt > 0.1) dt = 0.0;
                    float wx = event.vector.x;  // rad/s about the device axes
                    float wy = event.vector.y;
                    // Bias: while the phone is (almost) still, follow the sensor's offset.
                    if (fabsf(wx - sGyroBiasX) < 0.02f && fabsf(wy - sGyroBiasY) < 0.02f) {
                        sGyroBiasX += (wx - sGyroBiasX) * 0.003f;
                        sGyroBiasY += (wy - sGyroBiasY) * 0.003f;
                    }
                    wx -= sGyroBiasX;
                    wy -= sGyroBiasY;
                    // In landscape the screen's up axis is the device X axis and its
                    // right axis is the device -Y axis; sLandscapeSign flips both for
                    // the reversed landscape.  yaw > 0: turn right, pitch > 0: look up.
                    float yaw = -sLandscapeSign * wx;
                    float pitch = sLandscapeSign * wy;
                    if (cs.gyroInvertX) yaw = -yaw;
                    if (cs.gyroInvertY) pitch = -pitch;
                    const float dz = 0.012f;  // rad/s (under 1 deg/s): sensor noise
                    if (fabsf(yaw) < dz) yaw = 0.0f;
                    if (fabsf(pitch) < dz) pitch = 0.0f;
                    sGyroAccYaw += (double)yaw * dt;
                    sGyroAccPitch += (double)pitch * dt;
                    sGyroAccDt += dt;
                    sGyroCamNs = port_host_time_ns();
                }
            }
            if (mobile_ui::UIOverlay::get().settings().gyroAiming) {
                // Android Landscape coordinate conversion:
                float rotH = event.vector.x - event.vector.z * 0.35f;
                float rotV = event.vector.y;

                const auto& settings = mobile_ui::UIOverlay::get().settings();
                if (settings.gyroInvertX) rotH = -rotH;
                if (settings.gyroInvertY) rotV = -rotV;

                float sens = 0.035f;
                a.pad.pointerX += rotH * sens;
                a.pad.pointerY -= rotV * sens;
                a.pad.pointerX = std::max(-1.0f, std::min(1.0f, a.pad.pointerX));
                a.pad.pointerY = std::max(-1.0f, std::min(1.0f, a.pad.pointerY));
                a.pad.pointerValid = 1;
                auto& ui = mobile_ui::UIOverlay::get();
                ui.setPointerPos(a.pad.pointerX, a.pad.pointerY);
                a.pad.pointerX = ui.getPointerX();
                a.pad.pointerY = ui.getPointerY();
                port_input_set(0, &a.pad);
            }
        } else if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
            // Compute phone tilt/roll for motion steering (Ray Surfing, Star Ball)
            if (event.acceleration.x > 4.0f) sLandscapeSign = 1.0f;
            else if (event.acceleration.x < -4.0f) sLandscapeSign = -1.0f;
            float rawRoll = event.acceleration.x / 9.81f;
            float rawPitch = (event.acceleration.y - 4.9f) / 9.81f;
            rawRoll = std::max(-1.0f, std::min(1.0f, rawRoll));
            rawPitch = std::max(-1.0f, std::min(1.0f, rawPitch));

            // Apply physical tilt if virtual touch stick is not being actively dragged
            if (!mobile_ui::get().isStickActive()) {
                if (fabsf(rawRoll) > 0.08f || fabsf(rawPitch) > 0.08f) {
                    a.pad.tilted = 1;
                    a.pad.downX = rawRoll;
                    a.pad.downZ = rawPitch;
                    float rem = 1.0f - (a.pad.downX * a.pad.downX + a.pad.downZ * a.pad.downZ);
                    a.pad.downY = -(rem > 0.0f ? sqrtf(rem) : 0.0f);
                } else {
                    a.pad.tilted = 0;
                }
            } else {
                a.pad.tilted = 0;
            }

            if (mobile_ui::UIOverlay::get().settings().gyroAiming && !sGyroSensor) {
                float tiltX = -event.acceleration.y * 0.15f;
                float tiltY = event.acceleration.x * 0.15f;
                const auto& settings = mobile_ui::UIOverlay::get().settings();
                if (settings.gyroInvertX) tiltX = -tiltX;
                if (settings.gyroInvertY) tiltY = -tiltY;
                a.pad.pointerX = std::max(-1.0f, std::min(1.0f, tiltX));
                a.pad.pointerY = std::max(-1.0f, std::min(1.0f, tiltY));
                a.pad.pointerValid = 1;
                auto& ui = mobile_ui::UIOverlay::get();
                ui.setPointerPos(a.pad.pointerX, a.pad.pointerY);
                a.pad.pointerX = ui.getPointerX();
                a.pad.pointerY = ui.getPointerY();
            }
            port_input_set(0, &a.pad);
        }
    }
}

bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool checkGameFolder(const std::string& dir, bool* ready) {
    struct stat st;
    if (stat((dir + "/sys/fst.bin").c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !isDir(dir + "/files")) {
        return false;
    }
    char magic[4] = {};
    FILE* f = fopen((dir + "/files/ObjectData/Coin.arc").c_str(), "rb");
    bool converted = f && fread(magic, 1, 4, f) == 4 && memcmp(magic, "CRAR", 4) == 0;
    if (f) fclose(f);

    bool dolData = true;
    for (const char* name : {"ErrorMessageArchive.arc", "StoryEvent.bcsv", "GalaxyID.bcsv"}) {
        dolData = dolData && stat((dir + "/sys/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }
    bool known = port_dvd_identify(dir.c_str()) != nullptr;
    if (ready) *ready = converted && dolData && known;
    return true;
}

std::string findGameData(const std::string& extDir) {
    std::vector<std::string> candidates = {
        "/sdcard/GalaxyData",
        "/sdcard/GalaxyData/game",
        "/storage/emulated/0/GalaxyData",
        "/storage/emulated/0/GalaxyData/game",
        extDir + "/game",
        "/sdcard/Android/data/com.openport.galaxyandroid/files/game",
        "/sdcard/Android/data/com.galaxy.mobile.plus/files/game",
        "/sdcard/Android/data/com.galaxy.mobile/files/game",
        "/sdcard/Android/data/com.galaxy.quest/files/game",
        "/sdcard/Galaxy/game",
        "/sdcard/GalaxyQuest",
        "/sdcard/GalaxyQuest/game",
        "/sdcard/Download/game",
        "/sdcard/Download/cooked",
        "/sdcard/Download/GalaxyQuest",
        "/sdcard/Documents/game",
        "/sdcard/Documents/cooked",
        "/sdcard/game",
        "/sdcard/cooked",
        "/data/local/tmp/petari/game"
    };

    if (const char* envRoot = getenv("PETARI_GAME_ROOT")) {
        candidates.insert(candidates.begin(), envRoot);
    }

    for (const auto& path : candidates) {
        bool ready = false;
        if (checkGameFolder(path, &ready) && ready) {
            LOGI("Found valid game data at: %s", path.c_str());
            return path;
        }
    }
    return "";
}

void promptStoragePermissionIfMissing(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->clazz) return;
    JNIEnv* env = nullptr;
    if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;

    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass versionClass = env->FindClass("android/os/Build$VERSION");
    if (!versionClass) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
    jfieldID sdkIntField = env->GetStaticFieldID(versionClass, "SDK_INT", "I");
    if (!sdkIntField) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(versionClass);
        return;
    }
    jint sdkInt = env->GetStaticIntField(versionClass, sdkIntField);
    env->DeleteLocalRef(versionClass);

    if (sdkInt >= 30) {
        jclass envClass = env->FindClass("android/os/Environment");
        if (!envClass) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return;
        }
        jmethodID isManagerMethod = env->GetStaticMethodID(envClass, "isExternalStorageManager", "()Z");
        if (!isManagerMethod) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(envClass);
            return;
        }
        jboolean isManager = env->CallStaticBooleanMethod(envClass, isManagerMethod);
        env->DeleteLocalRef(envClass);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return;
        }

        if (!isManager) {
            LOGI("MANAGE_EXTERNAL_STORAGE permission missing, launching settings prompt...");
            jclass intentClass = env->FindClass("android/content/Intent");
            if (!intentClass) {
                if (env->ExceptionCheck()) env->ExceptionClear();
                return;
            }
            jmethodID intentCtor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");

            jclass uriClass = env->FindClass("android/net/Uri");
            if (!uriClass || !intentCtor) {
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (intentClass) env->DeleteLocalRef(intentClass);
                if (uriClass) env->DeleteLocalRef(uriClass);
                return;
            }
            jmethodID parseMethod = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");

            jclass actClass = env->GetObjectClass(activity->clazz);
            if (!actClass || !parseMethod) {
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(intentClass);
                env->DeleteLocalRef(uriClass);
                if (actClass) env->DeleteLocalRef(actClass);
                return;
            }
            jmethodID getPackageNameMethod = env->GetMethodID(actClass, "getPackageName", "()Ljava/lang/String;");
            jmethodID startActivityMethod = env->GetMethodID(actClass, "startActivity", "(Landroid/content/Intent;)V");

            if (getPackageNameMethod && startActivityMethod) {
                jstring pkgName = (jstring)env->CallObjectMethod(activity->clazz, getPackageNameMethod);
                if (pkgName) {
                    const char* pkgChars = env->GetStringUTFChars(pkgName, nullptr);
                    std::string uriStr = std::string("package:") + (pkgChars ? pkgChars : "");
                    if (pkgChars) env->ReleaseStringUTFChars(pkgName, pkgChars);

                    jstring jUriStr = env->NewStringUTF(uriStr.c_str());
                    jobject jUri = env->CallStaticObjectMethod(uriClass, parseMethod, jUriStr);

                    jstring actionStr = env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
                    jobject intent = env->NewObject(intentClass, intentCtor, actionStr, jUri);

                    if (intent) {
                        env->CallVoidMethod(activity->clazz, startActivityMethod, intent);
                        env->DeleteLocalRef(intent);
                    }
                    if (actionStr) env->DeleteLocalRef(actionStr);
                    if (jUri) env->DeleteLocalRef(jUri);
                    if (jUriStr) env->DeleteLocalRef(jUriStr);
                    env->DeleteLocalRef(pkgName);
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(actClass);
            env->DeleteLocalRef(uriClass);
            env->DeleteLocalRef(intentClass);
        }
    }
}

bool initEgl(MobileApp& a) {
    if (a.eglReady) return true;

    a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (a.display == EGL_NO_DISPLAY || !eglInitialize(a.display, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint cfgAttr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint count = 0;
    if (!eglChooseConfig(a.display, cfgAttr, &a.config, 1, &count) || count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint ctxAttr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE
    };

    a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, ctxAttr);
    if (a.context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (0x%x)", eglGetError());
        return false;
    }

    a.fxaaProgram = 0;
    a.fxaaVao = 0;
    a.fxaaImageLocation = -1;
    a.fxaaTexelLocation = -1;
    a.fxaaLevelLocation = -1;
    a.fxaaInitState = 0;
    a.eglReady = true;
    return true;
}

static void enforceLandscapeOrientation(MobileApp& a) {
    if (a.android && a.android->activity) {
        int orientationChoice = mobile_ui::UIOverlay::get().settings().screenOrientationChoice;
        // 0 = SCREEN_ORIENTATION_SENSOR_LANDSCAPE (6, auto-flips 180 deg)
        // 1 = SCREEN_ORIENTATION_LANDSCAPE (0, natural 90 deg)
        // 2 = SCREEN_ORIENTATION_REVERSE_LANDSCAPE (8, reverse 270 deg)
        int requested = 6;
        if (orientationChoice == 1) requested = 0;
        else if (orientationChoice == 2) requested = 8;

        ANativeActivity* act = a.android->activity;
        JNIEnv* env = nullptr;
        act->vm->AttachCurrentThread(&env, nullptr);
        if (env) {
            jclass actClass = env->GetObjectClass(act->clazz);
            if (actClass) {
                jmethodID setOrient = env->GetMethodID(actClass, "setRequestedOrientation", "(I)V");
                if (setOrient) {
                    env->CallVoidMethod(act->clazz, setOrient, requested);
                }
                env->DeleteLocalRef(actClass);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            act->vm->DetachCurrentThread();
        }
    }
}

void updateViewport(MobileApp& a) {
    if (mobile_ui::get().settings().fullscreen) {
        a.vpX = 0;
        a.vpY = 0;
        a.vpW = a.screenWidth;
        a.vpH = a.screenHeight;
    } else {
        float targetAspect = 16.0f / 9.0f;
        float screenAspect = (a.screenWidth >= a.screenHeight)
            ? ((float)a.screenWidth / (float)a.screenHeight)
            : ((float)a.screenHeight / (float)a.screenWidth);
        if (a.screenWidth >= a.screenHeight) {
            if (screenAspect > targetAspect) {
                a.vpH = a.screenHeight;
                a.vpW = (int)lroundf(a.screenHeight * targetAspect);
                a.vpX = (a.screenWidth - a.vpW) / 2;
                a.vpY = 0;
            } else {
                a.vpW = a.screenWidth;
                a.vpH = (int)lroundf(a.screenWidth / targetAspect);
                a.vpX = 0;
                a.vpY = (a.screenHeight - a.vpH) / 2;
            }
        } else {
            a.vpW = a.screenWidth;
            a.vpH = (int)lroundf(a.screenWidth / targetAspect);
            a.vpX = 0;
            a.vpY = (a.screenHeight - a.vpH) / 2;
        }
    }

    // Never enlarge beyond the fitted viewport. This setting is intentionally
    // a uniform scale so the game cannot be stretched or cropped by the user.
    static constexpr float kScreenScale[] = {0.85f, 0.95f, 1.0f};
    const int scaleIndex = std::max(0, std::min(2, mobile_ui::get().settings().screenScaleIndex));
    const float displayScale = kScreenScale[scaleIndex];
    if (displayScale < 0.999f) {
        a.vpW = std::max(1, (int)lroundf((float)a.vpW * displayScale));
        a.vpH = std::max(1, (int)lroundf((float)a.vpH * displayScale));
        a.vpX = (a.screenWidth - a.vpW) / 2;
        a.vpY = (a.screenHeight - a.vpH) / 2;
    }
}

bool createSurface(MobileApp& a) {
    if (!a.android->window || !a.eglReady) return false;

    EGLint format = 0;
    eglGetConfigAttrib(a.display, a.config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(a.android->window, 0, 0, format);

    a.surface = eglCreateWindowSurface(a.display, a.config, a.android->window, nullptr);
    if (a.surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed (0x%x)", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(a.display, a.surface, a.surface, a.context)) {
        LOGE("eglMakeCurrent failed (0x%x)", eglGetError());
        return false;
    }

    eglQuerySurface(a.display, a.surface, EGL_WIDTH, &a.screenWidth);
    eglQuerySurface(a.display, a.surface, EGL_HEIGHT, &a.screenHeight);
    enforceLandscapeOrientation(a);
    eglSwapInterval(a.display, 1);

    // OpenGL ES is what is running here: either the user asked for it or the
    // Vulkan device/driver turned out to be unusable.
    mobile_ui::get().setActiveBackend(0);

    updateViewport(a);
    {
        static int sSurfaceCount = 0;
        LOGI("Window surface #%d ready: %dx%d, viewport x=%d y=%d %dx%d (fullscreen=%d screenScale=%d) GL: %s",
             ++sSurfaceCount, a.screenWidth, a.screenHeight, a.vpX, a.vpY, a.vpW, a.vpH,
             mobile_ui::get().settings().fullscreen ? 1 : 0,
             mobile_ui::get().settings().screenScaleIndex, (const char*)glGetString(GL_RENDERER));
    }

    // Initialize renderer and render target
    gpu::Renderer& r = gpu::renderer();
    if (!r.init()) {
        LOGE("gpu::renderer().init() failed");
        return false;
    }

    // Allocate the selected quality, stepping down only if this GPU cannot
    // create it. The selected setting is retained, and the fallback is logged.
    const mobile_ui::Settings& settings = mobile_ui::get().settings();
    const int scaleChoice = effectiveRenderScale(settings);
    const RenderTargetSpec requested =
        renderTargetSpec(a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice);
    if (targetRequestChanged(a, scaleChoice, requested)) {
        int actualScale = -1;
        int targetW = 0, targetH = 0;
        gpu::EfbTarget newTarget = createTargetWithFallback<gpu::EfbTarget>(
            a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice, "GLES",
            actualScale, targetW, targetH,
            [&r](int width, int height) { return r.createTarget(width, height); },
            [](const gpu::EfbTarget& target) { return target.fbo != 0; });
        rememberTargetRequest(a, scaleChoice, requested);
        if (newTarget.fbo != 0) {
            if (a.targetReady) r.destroyTarget(a.efbTarget);
            a.efbTarget = newTarget;
            a.targetReady = true;
            a.activeRenderScale = actualScale;
            a.lastCompleteFrameValid = false;
            a.loggedFrameHold = false;
            LOGI("GLES target active: %dx%d quality=%d requested=%d fullscreen=%d",
                 targetW, targetH, actualScale, scaleChoice, settings.fullscreen ? 1 : 0);
        } else {
            LOGE("No GLES render target could be allocated down to 480p; retaining prior target if available");
            if (!a.targetReady) return false;
        }
    }

    a.windowReady = true;
    return true;
}

void destroySurface(MobileApp& a) {
    if (a.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(a.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (a.surface != EGL_NO_SURFACE) {
            eglDestroySurface(a.display, a.surface);
            a.surface = EGL_NO_SURFACE;
        }
    }
    a.windowReady = false;
}

static GLuint compileFxaaShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("FXAA shader compilation failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool ensureFxaaProgram(MobileApp& a) {
    if (a.fxaaInitState == 1 && glIsProgram(a.fxaaProgram) == GL_TRUE &&
        glIsVertexArray(a.fxaaVao) == GL_TRUE) return true;
    if (a.fxaaInitState < 0) return false;

    static const char* vertexSource = R"(#version 320 es
precision highp float;
out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static const char* fragmentSource = R"(#version 320 es
precision highp float;
in vec2 v_uv;
layout(location = 0) out vec4 outColor;
uniform sampler2D u_scene;
uniform vec2 u_texel;
uniform int u_level;

vec4 sampleScene(vec2 uv) {
    return texture(u_scene, clamp(uv, vec2(0.0), vec2(1.0)));
}

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec4 center = sampleScene(v_uv);
    if (u_level <= 0) {
        outColor = center;
        return;
    }

    float nw = luma(sampleScene(v_uv + vec2(-u_texel.x, -u_texel.y)).rgb);
    float ne = luma(sampleScene(v_uv + vec2( u_texel.x, -u_texel.y)).rgb);
    float sw = luma(sampleScene(v_uv + vec2(-u_texel.x,  u_texel.y)).rgb);
    float se = luma(sampleScene(v_uv + vec2( u_texel.x,  u_texel.y)).rgb);
    float m = luma(center.rgb);
    float minLuma = min(m, min(min(nw, ne), min(sw, se)));
    float maxLuma = max(m, max(max(nw, ne), max(sw, se)));
    float quality = clamp(float(u_level) / 12.0, 0.0, 1.0);
    float threshold = max(mix(0.125, 0.035, quality), maxLuma * mix(0.10, 0.045, quality));
    if (maxLuma - minLuma < threshold) {
        outColor = center;
        return;
    }

    vec2 direction = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduction = max((nw + ne + sw + se) * (0.25 * mix(0.125, 0.0625, quality)), 1.0 / 128.0);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduction),
                      vec2(-1.0), vec2(1.0));
    direction *= u_texel * mix(1.0, 6.0, quality);

    vec3 a = 0.5 * (
        sampleScene(v_uv + direction * (1.0 / 3.0 - 0.5)).rgb +
        sampleScene(v_uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (
        sampleScene(v_uv + direction * -0.5).rgb +
        sampleScene(v_uv + direction *  0.5).rgb);
    float lumaB = luma(b);
    vec3 result = (lumaB < minLuma || lumaB > maxLuma) ? a : b;
    outColor = vec4(result, center.a);
}
)";

    GLuint vs = compileFxaaShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileFxaaShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        a.fxaaInitState = -1;
        return false;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("FXAA program link failed: %s", log);
        glDeleteProgram(program);
        a.fxaaInitState = -1;
        return false;
    }

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    if (!vao) {
        glDeleteProgram(program);
        a.fxaaInitState = -1;
        LOGE("FXAA could not allocate its fullscreen-triangle VAO");
        return false;
    }

    a.fxaaProgram = program;
    a.fxaaVao = vao;
    a.fxaaImageLocation = glGetUniformLocation(program, "u_scene");
    a.fxaaTexelLocation = glGetUniformLocation(program, "u_texel");
    a.fxaaLevelLocation = glGetUniformLocation(program, "u_level");
    if (a.fxaaImageLocation < 0 || a.fxaaTexelLocation < 0 || a.fxaaLevelLocation < 0) {
        LOGE("FXAA program is missing a required uniform");
        glDeleteVertexArrays(1, &a.fxaaVao);
        glDeleteProgram(a.fxaaProgram);
        a.fxaaProgram = 0;
        a.fxaaVao = 0;
        a.fxaaInitState = -1;
        return false;
    }
    a.fxaaInitState = 1;
    LOGI("FXAA post-process ready (quality selector supports 0..12)");
    return true;
}

static bool presentGlesFxaa(MobileApp& a, int level) {
    if (level <= 0 || !a.targetReady || !a.efbTarget.color ||
        a.efbTarget.width <= 0 || a.efbTarget.height <= 0) return false;
    if (!ensureFxaaProgram(a)) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(a.vpX, a.vpY, a.vpW, a.vpH);
    glUseProgram(a.fxaaProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a.efbTarget.color);
    glBindVertexArray(a.fxaaVao);
    glUniform1i(a.fxaaImageLocation, 0);
    glUniform2f(a.fxaaTexelLocation, 1.0f / (float)a.efbTarget.width,
                1.0f / (float)a.efbTarget.height);
    glUniform1i(a.fxaaLevelLocation, std::max(0, std::min(12, level)));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const GLenum drawError = glGetError();
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glViewport(0, 0, a.screenWidth, a.screenHeight);
    if (drawError != GL_NO_ERROR) {
        LOGW("FXAA pass failed with GL error 0x%x; using unfiltered presentation", drawError);
        return false;
    }
    return true;
}

// Adaptive FXAA: when the game stays well below 60 fps (typically a GPU-bound
// 4K target plus top FXAA, made worse by the screen recorder), the FXAA quality
// is stepped down until the frame rate recovers, so the picture and the audio
// (which the game mixes frame by frame) stay smooth.  It climbs back one step at
// a time after 30 s of a steady 60 and never goes above the chosen quality.
extern "C" void port_crash_set_context(const char* text);
extern "C" float port_input_camera_sensitivity(void);
extern "C" void port_camera_reset(void);
static std::atomic<bool> sCameraResetRequested{false};
extern "C" void port_mobile_request_camera_reset(void) { sCameraResetRequested.store(true); }

// True while the FOV is applied through the game's own 3D camera (gameplay).
// Everywhere else (title, file select, movies) the FOV acts as a zoom of the image.
static bool sFovThroughCamera = false;

// Detailed breakdown, one line every 5 s of game time: how the frames went, what
// the quality governor did, memory/threads and which inputs were active.  The same
// numbers are kept in the crash report's CONTEXT line.  Every 30 s a "device" line
// adds battery, temperature, CPU clocks and free memory (whatever Android lets an
// app read), and a "session summary" is written when the app is paused or closed.
extern "C" const char* port_crash_scene(void);
extern "C" int port_audio_underruns(void);

struct PerfWindow {
    int64_t startNs = 0, lastNs = 0, sumNs = 0, maxNs = 0, minNs = 0;
    int frames = 0, slow20 = 0, slow33 = 0, slow100 = 0;
};
static PerfWindow sPerf;
static int sPerfWindowsLogged = 0;

struct SessionTotals {
    int64_t sumNs = 0, maxNs = 0, firstFrameNs = 0;
    int64_t frames = 0, slow20 = 0, slow33 = 0, slow100 = 0, stalls = 0;
    int adaptiveDown = 0, adaptiveUp = 0, lowMemory = 0;
    long peakRssMb = 0;
    int peakThreads = 0;
    std::string worstScene;
};
static SessionTotals sSession;
static int64_t sBootNs = 0;  // when the engine entry ran

static void readProcStatus(long* rssMb, int* threads) {
    *rssMb = -1;
    *threads = -1;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        long v = 0;
        if (sscanf(line, "VmRSS: %ld kB", &v) == 1) *rssMb = v / 1024;
        else if (sscanf(line, "Threads: %ld", &v) == 1) *threads = (int)v;
    }
    fclose(f);
}

// First line of a small sysfs/procfs file, or false when Android does not let the app read it.
static bool readSmallFile(const char* path, char* out, size_t size) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    const bool ok = fgets(out, (int)size, f) != nullptr;
    fclose(f);
    if (ok) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    }
    return ok;
}

static void logDeviceState() {
    char buf[96], status[48] = "n/a";
    long capacity = -1, tempTenths = LONG_MIN, currentUa = LONG_MIN;
    if (readSmallFile("/sys/class/power_supply/battery/capacity", buf, sizeof(buf))) capacity = atol(buf);
    if (readSmallFile("/sys/class/power_supply/battery/temp", buf, sizeof(buf))) tempTenths = atol(buf);
    if (readSmallFile("/sys/class/power_supply/battery/status", buf, sizeof(buf))) snprintf(status, sizeof(status), "%s", buf);
    if (readSmallFile("/sys/class/power_supply/battery/current_now", buf, sizeof(buf))) currentUa = atol(buf);

    long cpuMHz[3] = {-1, -1, -1};
    const int cpuIds[3] = {0, 4, 9};
    for (int i = 0; i < 3; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpuIds[i]);
        if (readSmallFile(path, buf, sizeof(buf))) cpuMHz[i] = atol(buf) / 1000;
    }

    long hottest = LONG_MIN;
    char hottestType[40] = "";
    int zonesRead = 0;
    for (int z = 0; z < 48; z++) {
        char path[96], type[40] = "";
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        if (!readSmallFile(path, buf, sizeof(buf))) continue;
        zonesRead++;
        long t = atol(buf);
        if (t > 1000) t /= 100;  // millidegrees -> tenths of a degree
        if (t > hottest) {
            hottest = t;
            snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
            readSmallFile(path, type, sizeof(type));
            snprintf(hottestType, sizeof(hottestType), "%s", type);
        }
    }

    long memAvailMb = -1, swapFreeMb = -1, memTotalMb = -1;
    if (FILE* f = fopen("/proc/meminfo", "r")) {
        char line[128];
        long v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemAvailable: %ld kB", &v) == 1) memAvailMb = v / 1024;
            else if (sscanf(line, "SwapFree: %ld kB", &v) == 1) swapFreeMb = v / 1024;
            else if (sscanf(line, "MemTotal: %ld kB", &v) == 1) memTotalMb = v / 1024;
        }
        fclose(f);
    }

    char bat[96], thermal[96], cpu[96];
    if (capacity >= 0) {
        if (tempTenths != LONG_MIN) snprintf(bat, sizeof(bat), "battery %ld%% %.1fC %s", capacity, tempTenths / 10.0, status);
        else snprintf(bat, sizeof(bat), "battery %ld%% %s", capacity, status);
    } else {
        snprintf(bat, sizeof(bat), "battery n/a (not readable by apps)");
    }
    if (currentUa != LONG_MIN) {
        const size_t n = strlen(bat);
        snprintf(bat + n, sizeof(bat) - n, " %ldmA", currentUa / 1000);
    }
    if (hottest != LONG_MIN) snprintf(thermal, sizeof(thermal), "hottest zone %.1fC (%s, %d zones readable)", hottest / 10.0, hottestType, zonesRead);
    else snprintf(thermal, sizeof(thermal), "thermal zones n/a");
    snprintf(cpu, sizeof(cpu), "cpu0=%ld cpu4=%ld cpu9=%ld MHz", cpuMHz[0], cpuMHz[1], cpuMHz[2]);
    LOGI("device 30s: %s | %s | %s | memory available %ldMB of %ldMB, swap free %ldMB | audio underruns %d",
         bat, thermal, cpu, memAvailMb, memTotalMb, swapFreeMb, port_audio_underruns());
}

static void logSessionSummary(const char* reason) {
    const SessionTotals& t = sSession;
    if (t.frames <= 0) {
        LOGI("session summary (%s): no game frames were presented", reason);
        return;
    }
    const double seconds = (double)t.sumNs / 1e9;
    LOGI("session summary (%s): %.0f s of game time, %lld frames, average %.1f fps (%.2f ms); slow frames >20ms %lld (%.1f%%), >33ms %lld (%.1f%%), "
         ">100ms %lld; worst frame %.0f ms (scene %s); FXAA governor steps down %d / up %d; audio underruns %d; low-memory warnings %d; "
         "peak rss %ldMB, peak threads %d; last scene %s",
         reason, seconds, (long long)t.frames, seconds > 0 ? t.frames / seconds : 0.0, (double)t.sumNs / t.frames / 1e6,
         (long long)t.slow20, 100.0 * t.slow20 / t.frames, (long long)t.slow33, 100.0 * t.slow33 / t.frames,
         (long long)t.slow100, t.maxNs / 1e6, t.worstScene.empty() ? "?" : t.worstScene.c_str(), t.adaptiveDown,
         t.adaptiveUp, port_audio_underruns(), t.lowMemory, t.peakRssMb, t.peakThreads, port_crash_scene());
}

static void perfOnGameFrame(MobileApp& a, int64_t nowNs, int fxaaEffective, int fxaaCap) {
    PerfWindow& w = sPerf;
    if (w.startNs == 0) {
        w.startNs = w.lastNs = nowNs;
        sSession.firstFrameNs = nowNs;
        LOGI("startup: first game frame %.2f s after the engine started (scene %s)",
             sBootNs ? (double)(nowNs - sBootNs) / 1e9 : -1.0, port_crash_scene());
        return;
    }
    const int64_t dt = nowNs - w.lastNs;
    w.lastNs = nowNs;
    if (dt <= 0) return;
    static int64_t sLastStallLogNs = 0;
    if (dt > 2000000000ll) {
        // The game was paused, or the app was in the background: not a slow frame.
        LOGI("perf: %.1f s gap between game frames (pause, background or a load); not counted as a slow frame",
             dt / 1e9);
        w.startNs = nowNs;
        return;
    }
    w.frames++;
    w.sumNs += dt;
    if (dt > w.maxNs) w.maxNs = dt;
    if (w.minNs == 0 || dt < w.minNs) w.minNs = dt;
    if (dt > 20000000ll) w.slow20++;
    if (dt > 33400000ll) w.slow33++;
    if (dt > 100000000ll) w.slow100++;

    SessionTotals& t = sSession;
    t.frames++;
    t.sumNs += dt;
    if (dt > 20000000ll) t.slow20++;
    if (dt > 33400000ll) t.slow33++;
    if (dt > 100000000ll) t.slow100++;
    if (dt > t.maxNs) { t.maxNs = dt; t.worstScene = port_crash_scene(); }
    if (dt > 150000000ll) {
        t.stalls++;
        if (nowNs - sLastStallLogNs > 1000000000ll) {
            sLastStallLogNs = nowNs;
            long rss; int thr;
            readProcStatus(&rss, &thr);
            LOGW("perf: frame stall %.0f ms before frame %llu (scene %s, rss=%ldMB, threads=%d, FXAA effective=%d)",
                 dt / 1e6, (unsigned long long)gPresentedFrames.load(), port_crash_scene(), rss, thr, fxaaEffective);
        }
    }
    if (nowNs - w.startNs < 5000000000ll) return;

    const mobile_ui::Settings& s = mobile_ui::get().settings();
    long rssMb; int threads;
    readProcStatus(&rssMb, &threads);
    if (rssMb > t.peakRssMb) t.peakRssMb = rssMb;
    if (threads > t.peakThreads) t.peakThreads = threads;
    const double avgMs = w.frames ? (double)w.sumNs / w.frames / 1e6 : 0.0;
    const double fps = w.sumNs ? w.frames * 1e9 / (double)w.sumNs : 0.0;
    char buf[900];
    snprintf(buf, sizeof(buf),
             "game %.1f fps (avg %.2f ms, min %.2f, max %.2f; >20ms %d, >33ms %d, >100ms %d of %d frames) | "
             "scene %s | FXAA chosen=%d cap=%d effective=%d adaptive=%s | res=%d speed=%dx | rss=%ldMB threads=%d | "
             "input: leftStick=%.2f,%.2f camStick=%.2f,%.2f rightStick=%.2f,%.2f gyroCam=%s gamepad=%s | "
             "freeCam=%s fov=%+d luigi=%s",
             fps, avgMs, w.minNs / 1e6, w.maxNs / 1e6, w.slow20, w.slow33, w.slow100, w.frames,
             port_crash_scene(),
             s.antialiasingLevel, fxaaCap, fxaaEffective, s.adaptiveQuality ? "on" : "off", s.renderScale, s.gameSpeed,
             rssMb, threads, a.pad.stickX, a.pad.stickY, mobile_ui::get().getCamStickX(), mobile_ui::get().getCamStickY(),
             a.pad.rightStickX, a.pad.rightStickY, s.gyroCamera ? "on" : "off", a.gamepadDetected ? "yes" : "no",
             s.modFreeCamera ? "on" : "off", s.fovOffset, s.modLuigi ? "on" : "off");
    LOGI("perf 5s #%d: %s", ++sPerfWindowsLogged, buf);
    port_crash_set_context(buf);
    if (sPerfWindowsLogged % 6 == 0) logDeviceState();
    w = PerfWindow();
    w.startNs = w.lastNs = nowNs;
}

static int sAdaptiveCap = 12;
static int sAdaptiveRequested = -1;
// Anti-hunting: a climb that is followed by a drop within kAdaptHuntWindowNs proves
// that level is too heavy for this device right now (GPU clocks back off as it
// heats up).  The level just below it becomes a ceiling until the chosen quality
// changes, so the load stops saw-toothing every minute or so.
static constexpr int64_t kAdaptHuntWindowNs = 180000000000ll;
static int sAdaptCeiling = 99;
static int64_t sAdaptLastUpNs = 0;
static int64_t sAdaptLastNs = 0;
static int64_t sAdaptAccumNs = 0;
static int sAdaptFrames = 0;
static int sAdaptGoodFrames = 0;
static int64_t sAdaptLastDownNs = 0;

static int adaptiveFxaaLevel(int requested) {
    if (!mobile_ui::get().settings().adaptiveQuality) {
        sAdaptiveRequested = requested;
        sAdaptiveCap = requested;  // switched off: exactly the chosen level, always
        return requested;
    }
    if (requested != sAdaptiveRequested) {
        sAdaptiveRequested = requested;
        sAdaptiveCap = requested;
        sAdaptGoodFrames = 0;
        sAdaptCeiling = 99;
        sAdaptLastUpNs = 0;
    }
    return std::min(requested, sAdaptiveCap);
}

static void adaptiveQualityOnGameFrame(int64_t nowNs) {
    const int requested = mobile_ui::get().settings().antialiasingLevel;
    const int64_t dt = nowNs - sAdaptLastNs;
    sAdaptLastNs = nowNs;
    if (!mobile_ui::get().settings().adaptiveQuality) {
        sAdaptAccumNs = 0;
        sAdaptFrames = 0;
        sAdaptGoodFrames = 0;
        return;
    }
    if (dt <= 0 || dt > 200000000ll || port_get_game_speed() > 1.05f || requested <= 0) {
        sAdaptAccumNs = 0;  // a pause, menu or speed-up: not a measurement
        sAdaptFrames = 0;
        return;
    }
    sAdaptAccumNs += dt;
    if (++sAdaptFrames < 90) return;
    const int64_t avg = sAdaptAccumNs / sAdaptFrames;
    const int level = adaptiveFxaaLevel(requested);
    if (avg > 21500000ll && sAdaptiveCap > 0) {  // under ~46 fps
        const int next = std::max(0, sAdaptiveCap - 3);
        if (sAdaptLastUpNs != 0 && nowNs - sAdaptLastUpNs < kAdaptHuntWindowNs) {
            sAdaptCeiling = std::min(sAdaptCeiling, std::max(0, sAdaptiveCap - 1));
            LOGI("adaptive quality: FXAA %d did not hold, ceiling now %d", sAdaptiveCap, sAdaptCeiling);
        }
        LOGI("adaptive quality: %.1f fps, FXAA %d -> %d (chosen %d)", 1e9 / (double)avg, level, next, requested);
        sAdaptiveCap = next;
        sSession.adaptiveDown++;
        sAdaptLastDownNs = nowNs;
        sAdaptGoodFrames = 0;
    } else if (avg < 17600000ll) {
        sAdaptGoodFrames += sAdaptFrames;
        if (sAdaptGoodFrames >= 1800 && sAdaptiveCap < std::min(requested, sAdaptCeiling) && nowNs - sAdaptLastDownNs > 30000000000ll) {
            sAdaptiveCap++;
            sAdaptLastUpNs = nowNs;
            sSession.adaptiveUp++;
            LOGI("adaptive quality: steady 60 fps, FXAA %d -> %d (chosen %d)", level, sAdaptiveCap, requested);
            sAdaptGoodFrames = 0;
        }
    } else {
        sAdaptGoodFrames = 0;
    }
    sAdaptAccumNs = 0;
    sAdaptFrames = 0;
}

// Draws the last complete game image into the back buffer: FXAA when it is
// enabled and available, otherwise a plain scaled blit. Every presentation path
// (new frame, UI redraw, high-refresh repeat, idle hold) goes through here so a
// repeated frame gets exactly the same filtering as a fresh one; without it the
// picture alternated between filtered and unfiltered at refresh rates above the
// game's 60 fps and whenever a menu forced a redraw.
static void drawGlesGameImage(MobileApp& a) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, a.screenWidth, a.screenHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    updateViewport(a);
    // FOV outside gameplay: no game camera to widen, so the whole picture is
    // scaled about its centre (negative = closer, positive = farther).
    const int savedVpX = a.vpX, savedVpY = a.vpY, savedVpW = a.vpW, savedVpH = a.vpH;
    const int fovMenu = mobile_ui::get().settings().fovOffset;
    if (fovMenu != 0 && !sFovThroughCamera && !mobile_ui::get().settings().modFreeCamera) {
        const float kDeg = 3.14159265f / 180.0f;
        const float z = tanf(22.5f * kDeg) / tanf((45.0f + (float)fovMenu) * 0.5f * kDeg);
        const float cx = a.vpX + a.vpW * 0.5f, cy = a.vpY + a.vpH * 0.5f;
        a.vpW = std::max(8, (int)lroundf(a.vpW * z));
        a.vpH = std::max(8, (int)lroundf(a.vpH * z));
        a.vpX = (int)lroundf(cx - a.vpW * 0.5f);
        a.vpY = (int)lroundf(cy - a.vpH * 0.5f);
    }
    struct VpRestore {
        MobileApp& m; int x, y, w, h;
        ~VpRestore() { m.vpX = x; m.vpY = y; m.vpW = w; m.vpH = h; }
    } vpRestore{a, savedVpX, savedVpY, savedVpW, savedVpH};
    const int aaLevel = adaptiveFxaaLevel(mobile_ui::get().settings().antialiasingLevel);
    if (!presentGlesFxaa(a, aaLevel)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, a.efbTarget.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, a.efbTarget.width, a.efbTarget.height,
                          a.vpX, a.vpY, a.vpX + a.vpW, a.vpY + a.vpH,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, a.screenWidth, a.screenHeight);
    }
}

void presentEglSurface(MobileApp& a) {
    static EGLint lastSwapError = EGL_SUCCESS;
    // The GX EFB alpha channel is game-internal data (including destination
    // alpha), not transparency for Android's window compositor. Keep the
    // full-screen NativeActivity surface opaque without changing any RGB
    // pixels, so an RGBA EGL config cannot show content from behind the game.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (eglSwapBuffers(a.display, a.surface) != EGL_TRUE) {
        const EGLint error = eglGetError();
        if (error != lastSwapError) {
            LOGE("eglSwapBuffers failed: 0x%x", error);
        }
        lastSwapError = error;
    } else if (lastSwapError != EGL_SUCCESS) {
        LOGI("eglSwapBuffers recovered after error 0x%x", lastSwapError);
        lastSwapError = EGL_SUCCESS;
    }
}

int queryConnectedGamepad(android_app* app) {
    if (!app || !app->activity || !app->activity->vm || !app->activity->clazz) return -1;
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    bool detachAfter = false;
    const jint envStatus = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envStatus == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return -1;
        detachAfter = true;
    } else if (envStatus != JNI_OK || !env) {
        return -1;
    }

    int result = -1;
    jclass activityClass = env->GetObjectClass(app->activity->clazz);
    jmethodID method = activityClass
        ? env->GetMethodID(activityClass, "hasConnectedGamepad", "()Z")
        : nullptr;
    if (method) {
        const jboolean connected = env->CallBooleanMethod(app->activity->clazz, method);
        if (!env->ExceptionCheck()) {
            result = connected == JNI_TRUE ? 1 : 0;
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (activityClass) env->DeleteLocalRef(activityClass);
    if (detachAfter) vm->DetachCurrentThread();
    return result;
}

void setGamepadConnection(MobileApp& app, bool connected, const char* source,
                          int deviceId = -1) {
    if (app.gamepadDetected == connected) return;
    app.gamepadDetected = connected;
    mobile_ui::get().setGamepadConnected(connected);
    if (connected) {
        mobile_ui::get().showToast("Mando conectado: controles tactiles ocultos");
        LOGI("controller: connected (%s), device=%d; touch controls hidden",
             source ? source : "input", deviceId);
    } else {
        app.physicalButtons = 0;
        app.physicalAxisButtons = 0;
        app.physicalMotionButtons = 0;
        app.physicalStickX = 0.0f;
        app.physicalStickY = 0.0f;
        app.pad.stickX = app.pad.stickY = 0.0f;
        app.pad.rightStickX = app.pad.rightStickY = 0.0f;
        app.pad.buttons = combinedPadButtons(app);
        port_input_set(0, &app.pad);
        mobile_ui::get().showToast("Mando desconectado: controles tactiles restaurados");
        LOGI("controller: disconnected (%s), device=%d; touch controls restored",
             source ? source : "device scan", deviceId);
    }
}

void refreshGamepadConnection(MobileApp& app) {
    const int64_t nowNs = port_host_time_ns();
    if (nowNs < app.nextControllerPollNs) return;
    app.nextControllerPollNs = nowNs + 500000000ll;
    const int connected = queryConnectedGamepad(app.android);
    if (connected >= 0) {
        setGamepadConnection(app, connected != 0, "Android InputDevice scan");
    } else {
        static bool loggedUnavailable = false;
        if (!loggedUnavailable) {
            LOGW("controller: Android device scan unavailable; retaining input-event detection");
            loggedUnavailable = true;
        }
    }
}

int32_t onInput(android_app* app, AInputEvent* event) {
    MobileApp* a = (MobileApp*)app->userData;
    if (!a) return 0;

    int32_t eventType = AInputEvent_getType(event);
    if (eventType == AINPUT_EVENT_TYPE_MOTION) {
        int32_t source = AInputEvent_getSource(event);
        bool isJoystickSource = (source & AINPUT_SOURCE_CLASS_JOYSTICK) != 0 ||
                                ((source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) ||
                                ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK);

        // Physical gamepad analog stick, D-pad, and trigger mapping
        if (isJoystickSource) {
            if (!a->gamepadDetected) {
                setGamepadConnection(*a, true, "motion event",
                                     AInputEvent_getDeviceId(event));
            }

            // Left Stick (Analog Movement)
            float stickX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
            float stickY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
            float stickMag = sqrtf(stickX * stickX + stickY * stickY);
            const auto& inputSettings = mobile_ui::get().settings();
            const float kDeadzone = std::max(0.05f, std::min(0.40f, inputSettings.controllerDeadzone));
            const float leftSensitivity = std::max(0.25f, std::min(2.0f, inputSettings.leftStickSensitivity));
            if (stickMag <= kDeadzone) {
                stickX = 0.0f;
                stickY = 0.0f;
            } else {
                float norm = (stickMag - kDeadzone) / (1.0f - kDeadzone);
                stickX = (stickX / stickMag) * std::min(1.0f, norm);
                stickY = (stickY / stickMag) * std::min(1.0f, norm);
            }
            stickX = std::max(-1.0f, std::min(1.0f, stickX * leftSensitivity));
            stickY = std::max(-1.0f, std::min(1.0f, stickY * leftSensitivity));
            const int64_t eventTimeNs = port_host_time_ns();
            static int64_t lastActiveLeftAxisLogNs = 0;
            if (stickMag > kDeadzone && eventTimeNs - lastActiveLeftAxisLogNs > 1500000000LL) {
                LOGI("controller: active left stick device=%d raw=%.2f,%.2f output=%.2f,%.2f deadzone=%.2f sensitivity=%.2f",
                     AInputEvent_getDeviceId(event),
                     AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0),
                     AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0),
                     stickX, -stickY, kDeadzone, inputSettings.leftStickSensitivity);
                lastActiveLeftAxisLogNs = eventTimeNs;
            }

            a->pad.connected = 1;
            a->physicalStickX = stickX;
            a->physicalStickY = -stickY; // Invert Y for Wii/GameCube convention
            if (mobile_ui::get().isStickActive()) {
                a->pad.stickX = mobile_ui::get().getTouchStickX();
                a->pad.stickY = mobile_ui::get().getTouchStickY();
            } else if (a->gamepadDetected && (fabsf(a->physicalStickX) > 0.001f || fabsf(a->physicalStickY) > 0.001f)) {
                a->pad.stickX = a->physicalStickX;
                a->pad.stickY = a->physicalStickY;
            } else {
                a->pad.stickX = 0.0f;
                a->pad.stickY = 0.0f;
            }

            // D-Pad Hat axes (Standard on Xbox, PlayStation, 8BitDo, and HID pads)
            float hatX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hatY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
            if (hatX < -0.5f) { a->physicalAxisButtons |= 0x0001; a->physicalAxisButtons &= ~0x0002; }
            else if (hatX > 0.5f) { a->physicalAxisButtons |= 0x0002; a->physicalAxisButtons &= ~0x0001; }
            else { a->physicalAxisButtons &= ~0x0003; }

            if (hatY < -0.5f) { a->physicalAxisButtons |= 0x0008; a->physicalAxisButtons &= ~0x0004; }
            else if (hatY > 0.5f) { a->physicalAxisButtons |= 0x0004; a->physicalAxisButtons &= ~0x0008; }
            else { a->physicalAxisButtons &= ~0x000C; }

            // Analog Triggers (L2 / R2)
            float rawL = std::max(0.0f, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0));
            float rawBrake = std::max(0.0f, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0));
            float lTrig = std::max(rawL, rawBrake);

            float rawR = std::max(0.0f, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0));
            float rawGas = std::max(0.0f, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0));
            float rTrig = std::max(rawR, rawGas);

            if (lTrig > 0.4f) a->physicalAxisButtons |= 0x2000; // Z button (Crouch / Ground Pound)
            else a->physicalAxisButtons &= ~0x2000;

            if (rTrig > 0.4f) a->spinFrames = 6; // Spin shake

            // A few HID pads report face buttons on joystick motion events
            // instead of emitting AKEYCODE_BUTTON_* key events.
            const int32_t motionButtonState = AMotionEvent_getButtonState(event);
            uint32_t motionButtons = 0;
            if (motionButtonState & AMOTION_EVENT_BUTTON_PRIMARY) {
                motionButtons |= 0x0800;  // Confirm / A
            }
            if (motionButtonState & AMOTION_EVENT_BUTTON_SECONDARY) {
                motionButtons |= 0x0400;  // Back / B
            }
            if (motionButtons != a->physicalMotionButtons) {
                LOGI("controller: motion buttons device=%d raw=0x%x mapped=0x%04x",
                     AInputEvent_getDeviceId(event), motionButtonState,
                     motionButtons);
            }
            a->physicalMotionButtons = motionButtons;

            // Android gamepads commonly use RX/RY for the right stick; use
            // Z/RZ only as a fallback for devices with the alternate HID map.
            const float rawRx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RX, 0);
            const float rawRy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RY, 0);
            const float rawZ = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
            const float rawRz = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
            float rightX = rawRx;
            float rightY = rawRy;
            const char* rightAxisPair = "RX/RY";
            if (fabsf(rightX) <= 0.05f && fabsf(rightY) <= 0.05f) {
                rightX = rawZ;
                rightY = rawRz;
                rightAxisPair = "Z/RZ";
            }
            static int lastAxisLogDevice = -1;
            static int64_t lastActiveAxisLogNs = 0;
            const int deviceId = AInputEvent_getDeviceId(event);
            if (deviceId != lastAxisLogDevice) {
                LOGI("controller: axis sample device=%d source=0x%x X=%.2f Y=%.2f RX=%.2f RY=%.2f Z=%.2f RZ=%.2f",
                     deviceId, source,
                     AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0),
                     AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0),
                     rawRx, rawRy, rawZ, rawRz);
                lastAxisLogDevice = deviceId;
            }
            const float rStickDeadzone = std::max(kDeadzone, 0.12f);
            const float rightMag = sqrtf(rightX * rightX + rightY * rightY);
            float rStickX = 0.0f;
            float rStickY = 0.0f;
            if (rightMag > rStickDeadzone) {
                const float normalized = std::min(1.0f, (rightMag - rStickDeadzone) / (1.0f - rStickDeadzone));
                const float gain = normalized / rightMag;
                rStickX = std::max(-1.0f, std::min(1.0f, rightX * gain));
                rStickY = std::max(-1.0f, std::min(1.0f, -rightY * gain));
            }
            if (rightMag > rStickDeadzone && eventTimeNs - lastActiveAxisLogNs > 1500000000LL) {
                LOGI("controller: active camera stick device=%d axes=%s raw=%.2f,%.2f output=%.2f,%.2f deadzone=%.2f sensitivity=%.2f",
                     deviceId, rightAxisPair, rightX, rightY, rStickX, rStickY,
                     rStickDeadzone, inputSettings.rightStickSensitivity);
                lastActiveAxisLogNs = eventTimeNs;
            }
            a->pad.rightStickX = rStickX;
            a->pad.rightStickY = rStickY;

            // The right stick is reserved for Mario's gameplay camera. Aim and
            // menu pointing stay on touch; don't route this stick to Star Pointer.

            a->pad.buttons = combinedPadButtons(*a);
            port_input_set(0, &a->pad);
            return 1;
        }

        int32_t touchAction = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        if (touchAction == AMOTION_EVENT_ACTION_DOWN) a->touchscreenDown = true;
        if (touchAction == AMOTION_EVENT_ACTION_UP || touchAction == AMOTION_EVENT_ACTION_CANCEL) a->touchscreenDown = false;

        updateViewport(*a);
        int ptrX = a->vpX, ptrY = a->vpY, ptrW = a->vpW, ptrH = a->vpH;
        if (mobile_ui::get().settings().fullscreen) {
            ptrW = (int)lroundf((float)a->screenHeight * (16.0f / 9.0f));
            ptrX = (a->screenWidth - ptrW) / 2;
        }
        if (mobile_ui::get().handleInput(event, a->pad, a->spinFrames,
                                         a->screenWidth, a->screenHeight,
                                         ptrX, ptrY, ptrW, ptrH)) {
            if (mobile_ui::get().isStickActive()) {
                a->pad.stickX = mobile_ui::get().getTouchStickX();
                a->pad.stickY = mobile_ui::get().getTouchStickY();
            } else if (a->gamepadDetected && (fabsf(a->physicalStickX) > 0.001f || fabsf(a->physicalStickY) > 0.001f)) {
                a->pad.stickX = a->physicalStickX;
                a->pad.stickY = a->physicalStickY;
            } else {
                a->pad.stickX = 0.0f;
                a->pad.stickY = 0.0f;
            }
            a->pad.buttons = combinedPadButtons(*a);
            port_input_set(0, &a->pad);
            return 1;
        }
        return 0;
    } else if (eventType == AINPUT_EVENT_TYPE_KEY) {
        int32_t keyCode = AKeyEvent_getKeyCode(event);
        // Do not intercept system volume keys: let Android OS adjust system volume normally
        if (keyCode == AKEYCODE_VOLUME_UP || keyCode == AKEYCODE_VOLUME_DOWN || keyCode == AKEYCODE_VOLUME_MUTE) {
            return 0;
        }

        int32_t keyAction = AKeyEvent_getAction(event);
        bool down = (keyAction == AKEY_EVENT_ACTION_DOWN);
        if (mobile_ui::get().captureControllerKey(keyCode, down)) {
            if (!a->gamepadDetected) {
                setGamepadConnection(*a, true, "key capture",
                                     AInputEvent_getDeviceId(event));
            }
            a->pad.connected = 1;
            a->pad.buttons = combinedPadButtons(*a);
            port_input_set(0, &a->pad);
            return 1;
        }

        const bool controllerButtonKey = (keyCode >= AKEYCODE_BUTTON_A && keyCode <= AKEYCODE_BUTTON_MODE) ||
                                         (keyCode >= AKEYCODE_BUTTON_1 && keyCode <= AKEYCODE_BUTTON_16);
        const bool controllerDpadKey = keyCode == AKEYCODE_DPAD_UP || keyCode == AKEYCODE_DPAD_DOWN ||
                                       keyCode == AKEYCODE_DPAD_LEFT || keyCode == AKEYCODE_DPAD_RIGHT ||
                                       keyCode == AKEYCODE_DPAD_CENTER;
        if (!a->gamepadDetected && (controllerButtonKey || controllerDpadKey)) {
            setGamepadConnection(*a, true, "key event",
                                 AInputEvent_getDeviceId(event));
        }
        if ((controllerButtonKey || controllerDpadKey) &&
            (!down || AKeyEvent_getRepeatCount(event) == 0)) {
            LOGI("controller: key event code=%d action=%s repeat=%d device=%d source=0x%x",
                 keyCode, down ? "down" : "up",
                 AKeyEvent_getRepeatCount(event),
                 AInputEvent_getDeviceId(event), AInputEvent_getSource(event));
        }

        const mobile_ui::Settings& controllerSettings = mobile_ui::get().settings();
        const int customKeys[6] = {
            controllerSettings.controllerKeyA, controllerSettings.controllerKeyB,
            controllerSettings.controllerKeyC, controllerSettings.controllerKeyZ,
            controllerSettings.controllerKeySpin, controllerSettings.controllerKeyPause
        };
        bool customKeyMatched = false;
        uint32_t customMask = 0;
        bool customSpin = false;
        for (int i = 0; i < 6; ++i) {
            if (customKeys[i] != keyCode) continue;
            customKeyMatched = true;
            if (i == 0) customMask |= 0x0800;       // A
            else if (i == 1) customMask |= 0x0400;  // B
            else if (i == 2) customMask |= 0x1000;  // C
            else if (i == 3) customMask |= 0x2000;  // Z
            else if (i == 4) customSpin = true;     // Spin pulse
            else if (i == 5) customMask |= 0x0010;  // Pause
        }

        if (customKeyMatched) {
            a->pad.connected = 1;
            if (down) {
                a->physicalButtons |= customMask;
                if (customSpin) a->spinFrames = 6;
            } else {
                a->physicalButtons &= ~customMask;
            }
        } else switch (keyCode) {
        case AKEYCODE_BUTTON_A:
        case AKEYCODE_BUTTON_1:
        case AKEYCODE_DPAD_CENTER:
        case AKEYCODE_ENTER:
        case AKEYCODE_NUMPAD_ENTER:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0800;  // A
            else a->physicalButtons &= ~0x0800;
            break;
        case AKEYCODE_BUTTON_B:
        case AKEYCODE_BUTTON_2:
        case AKEYCODE_BUTTON_C:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0400;  // B
            else a->physicalButtons &= ~0x0400;
            break;
        case AKEYCODE_BUTTON_X:
        case AKEYCODE_BUTTON_3:
            a->pad.connected = 1;
            if (down) a->spinFrames = 6;  // Spin (Wiimote shake)
            break;
        case AKEYCODE_BUTTON_Y:
        case AKEYCODE_BUTTON_4:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x1000;  // C
            else a->physicalButtons &= ~0x1000;
            break;
        case AKEYCODE_BUTTON_L1:
        case AKEYCODE_BUTTON_5:
        case AKEYCODE_BUTTON_L2:
        case AKEYCODE_BUTTON_7:
        case AKEYCODE_BUTTON_THUMBL:
        case AKEYCODE_BUTTON_11:
        case AKEYCODE_BUTTON_Z:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x2000;  // Z (crouch / ground pound)
            else a->physicalButtons &= ~0x2000;
            break;
        case AKEYCODE_BUTTON_R1:
        case AKEYCODE_BUTTON_6:
        case AKEYCODE_BUTTON_R2:
        case AKEYCODE_BUTTON_8:
        case AKEYCODE_BUTTON_THUMBR:
        case AKEYCODE_BUTTON_12:
            a->pad.connected = 1;
            if (down) a->spinFrames = 6;  // Spin (Wiimote shake)
            break;
        case AKEYCODE_BUTTON_START:
        case AKEYCODE_BUTTON_10:
        case AKEYCODE_BUTTON_SELECT:
        case AKEYCODE_BUTTON_9:
        case AKEYCODE_BUTTON_MODE:
        case AKEYCODE_MENU:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0010;  // Plus / Pause
            else a->physicalButtons &= ~0x0010;
            break;
        case AKEYCODE_DPAD_UP:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0008;
            else a->physicalButtons &= ~0x0008;
            break;
        case AKEYCODE_DPAD_DOWN:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0004;
            else a->physicalButtons &= ~0x0004;
            break;
        case AKEYCODE_DPAD_LEFT:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0001;
            else a->physicalButtons &= ~0x0001;
            break;
        case AKEYCODE_DPAD_RIGHT:
            a->pad.connected = 1;
            if (down) a->physicalButtons |= 0x0002;
            else a->physicalButtons &= ~0x0002;
            break;
        case AKEYCODE_BACK:
        case AKEYCODE_ESCAPE:
            if (down) {
                if (mobile_ui::get().isSettingsOpen()) {
                    mobile_ui::get().closeSettings();
                } else {
                    a->physicalButtons |= 0x0010; // Pause (+)
                }
            } else {
                a->physicalButtons &= ~0x0010;
            }
            return 1;
        default:
            return 0;
        }

        if (mobile_ui::get().isStickActive()) {
            a->pad.stickX = mobile_ui::get().getTouchStickX();
            a->pad.stickY = mobile_ui::get().getTouchStickY();
        } else if (a->gamepadDetected && (fabsf(a->physicalStickX) > 0.001f || fabsf(a->physicalStickY) > 0.001f)) {
            a->pad.stickX = a->physicalStickX;
            a->pad.stickY = a->physicalStickY;
        } else {
            a->pad.stickX = 0.0f;
            a->pad.stickY = 0.0f;
        }
        a->pad.buttons = combinedPadButtons(*a);
        port_input_set(0, &a->pad);
        return 1;
    }

    return 0;
}

// Brings the Vulkan backend up on *window* and creates the offscreen target at
// the configured render scale.  Shared by startup and by the recovery that runs
// when a device is lost mid-session, so a recreated device goes through exactly
// the same setup as the first one.
static bool bringUpVulkan(MobileApp& a, ANativeWindow* window) {
    if (!window) return false;
    if (!gpu::VkRenderer::get().init(window)) return false;

    a.vkReady = true;
    mobile_ui::get().setActiveBackend(1);
    mobile_ui::get().setGpuRendererName(gpu::VkRenderer::get().deviceName());
    a.screenWidth = ANativeWindow_getWidth(window);
    a.screenHeight = ANativeWindow_getHeight(window);
    enforceLandscapeOrientation(a);
    a.windowReady = true;

    const mobile_ui::Settings& settings = mobile_ui::get().settings();
    const int scaleChoice = effectiveRenderScale(settings);
    const RenderTargetSpec requested =
        renderTargetSpec(a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice);
    int actualScale = -1;
    int targetW = 0, targetH = 0;
    gpu::VkTarget newTarget = createTargetWithFallback<gpu::VkTarget>(
        a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice, "Vulkan",
        actualScale, targetW, targetH,
        [](int width, int height) {
            return gpu::VkRenderer::get().createTarget(width, height);
        },
        [](const gpu::VkTarget& target) { return target.framebuffer != VK_NULL_HANDLE; });
    if (newTarget.framebuffer != VK_NULL_HANDLE) {
        if (a.vkTarget.framebuffer) gpu::VkRenderer::get().destroyTarget(a.vkTarget);
        a.vkTarget = newTarget;
        a.targetReady = true;
        a.activeRenderScale = actualScale;
        rememberTargetRequest(a, scaleChoice, requested);
    } else if (!a.vkTarget.framebuffer) {
        LOGE("Vulkan could not allocate any render target from 4K down to 480p");
        gpu::VkRenderer::get().shutdown();
        a.vkReady = false;
        a.targetReady = false;
        return false;
    }
    updateViewport(a);
    LOGI("Vulkan target active: %dx%d quality=%d requested=%d fullscreen=%d",
         targetW, targetH, actualScale, scaleChoice, settings.fullscreen ? 1 : 0);
    return true;
}

void onAppCmd(android_app* app, int32_t cmd) {
    MobileApp* a = (MobileApp*)app->userData;
    if (!a) return;

    switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
        bool useVulkan = (mobile_ui::get().settings().graphicsBackend == 1);
        if (useVulkan) {
            LOGI("Selected Vulkan 1.0 backend, initializing VkRenderer with native touch controls...");
            if (!bringUpVulkan(*a, app->window)) {
                LOGE("Vulkan 1.0 initialization failed; falling back to OpenGL ES 3.2");
                a->vkReady = false;
                initEgl(*a);
                createSurface(*a);
            }
        } else {
            LOGI("Selected OpenGL ES 3.2 compatibility backend");
            initEgl(*a);
            createSurface(*a);
        }
        break;
    }
    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");
        if (a->vkReady) {
            if (a->vkTarget.framebuffer) {
                gpu::VkRenderer::get().destroyTarget(a->vkTarget);
            }
            gpu::VkRenderer::get().shutdown();
            a->vkReady = false;
            a->windowReady = false;
            a->targetReady = false;
            a->requestedRenderScale = -1;
        } else {
            destroySurface(*a);
        }
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        LOGI("APP_CMD_WINDOW_RESIZED / CONFIG_CHANGED");
        if (a->vkReady) {
            gpu::VkRenderer::get().onWindowResized(app->window);
            a->screenWidth = ANativeWindow_getWidth(app->window);
            a->screenHeight = ANativeWindow_getHeight(app->window);
            updateViewport(*a);
        } else if (a->surface != EGL_NO_SURFACE) {
            destroySurface(*a);
            createSurface(*a);
        }
        break;
    case APP_CMD_RESUME:
        LOGI("APP_CMD_RESUME");
        sAppPaused = false;
        mobile_ui::get().resumeAchievementSession();
        port_audio_resume();
        port_set_paused(0);
        break;
    case APP_CMD_PAUSE:
        LOGI("APP_CMD_PAUSE");
        sAppPaused = true;
        logSessionSummary("paused");
        // Key-up events are lost while the activity is paused, so drop every held
        // button instead of resuming with a stuck pad.
        a->physicalButtons = 0;
        a->physicalAxisButtons = 0;
        a->physicalMotionButtons = 0;
        a->spinFrames = 0;
        a->touchscreenDown = false;
        a->pad.buttons = combinedPadButtons(*a);
        port_input_set(0, &a->pad);
        mobile_ui::get().pauseAchievementSession();
        port_audio_pause();
        port_set_paused(1);
        break;
    case APP_CMD_LOW_MEMORY:
        sSession.lowMemory++;
        LOGW("APP_CMD_LOW_MEMORY: Android reported memory pressure; active render quality=%d target=%dx%d",
             a->activeRenderScale,
             a->vkReady ? a->vkTarget.width : a->efbTarget.width,
             a->vkReady ? a->vkTarget.height : a->efbTarget.height);
        break;
    case APP_CMD_DESTROY:
        LOGI("APP_CMD_DESTROY");
        logSessionSummary("closing");
        mobile_ui::get().pauseAchievementSession();
        if (a->vkReady) {
            if (a->vkTarget.framebuffer) {
                gpu::VkRenderer::get().destroyTarget(a->vkTarget);
            }
            gpu::VkRenderer::get().shutdown();
            a->vkReady = false;
            a->windowReady = false;
        } else {
            destroySurface(*a);
        }
        break;
    default:
        break;
    }
}

}  // namespace

extern "C" void port_android_vibrate(int ms) {
    if (ms <= 0) return;
    if (!gMobile.android || !gMobile.android->activity || !gMobile.android->activity->vm || !gMobile.android->activity->clazz) return;
    JavaVM* vm = gMobile.android->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint getEnvRes = vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
            return;
        }
        attached = true;
    } else if (getEnvRes != JNI_OK || !env) {
        return;
    }

    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass clazz = env->GetObjectClass(gMobile.android->activity->clazz);
    if (clazz) {
        jmethodID mid = env->GetStaticMethodID(clazz, "triggerHaptic", "(I)V");
        if (mid) {
            env->CallStaticVoidMethod(clazz, mid, (jint)ms);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(clazz);
    }

    if (attached) vm->DetachCurrentThread();
}

extern "C" void port_android_exit_to_launcher() {
    if (!gMobile.android || !gMobile.android->activity || !gMobile.android->activity->vm || !gMobile.android->activity->clazz) return;
    JavaVM* vm = gMobile.android->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint getEnvRes = vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
            return;
        }
        attached = true;
    } else if (getEnvRes != JNI_OK || !env) {
        return;
    }

    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass clazz = env->GetObjectClass(gMobile.android->activity->clazz);
    if (clazz) {
        jmethodID mid = env->GetStaticMethodID(clazz, "returnToLauncher", "()V");
        if (mid) {
            env->CallStaticVoidMethod(clazz, mid);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(clazz);
    }

    if (attached) vm->DetachCurrentThread();
}

extern "C" int port_android_copy_to_clipboard(const char* bytes, int length) {
    if (!bytes || length < 0 || (size_t)length >= PORT_LOG_BUFFER_CAPACITY) return 0;
    if (!gMobile.android || !gMobile.android->activity ||
        !gMobile.android->activity->vm || !gMobile.android->activity->clazz) return 0;

    JavaVM* vm = gMobile.android->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint getEnvRes = vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return 0;
        attached = true;
    } else if (getEnvRes != JNI_OK || !env) {
        return 0;
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    bool success = false;
    jclass clazz = env->GetObjectClass(gMobile.android->activity->clazz);
    if (clazz) {
        jmethodID method = env->GetStaticMethodID(clazz, "copyDiagnosticsToClipboard", "([B)V");
        if (method && !env->ExceptionCheck()) {
            jbyteArray data = env->NewByteArray(length);
            if (data) {
                if (length > 0) {
                    env->SetByteArrayRegion(data, 0, length, reinterpret_cast<const jbyte*>(bytes));
                }
                if (!env->ExceptionCheck()) {
                    env->CallStaticVoidMethod(clazz, method, data);
                    success = !env->ExceptionCheck();
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(data);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(clazz);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (attached) vm->DetachCurrentThread();
    return success ? 1 : 0;
}

extern "C" int port_android_choose_mod_zip(const char* modsDirectory) {
    if (!modsDirectory || !modsDirectory[0] || !gMobile.android ||
        !gMobile.android->activity || !gMobile.android->activity->vm ||
        !gMobile.android->activity->clazz) {
        return 0;
    }

    JavaVM* vm = gMobile.android->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return 0;
        attached = true;
    } else if (status != JNI_OK || !env) {
        return 0;
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    bool requested = false;
    jclass activityClass = env->GetObjectClass(gMobile.android->activity->clazz);
    if (activityClass) {
        jmethodID method = env->GetStaticMethodID(
                activityClass, "requestModsZipImport", "(Ljava/lang/String;)V");
        if (method && !env->ExceptionCheck()) {
            jstring directory = env->NewStringUTF(modsDirectory);
            if (directory) {
                env->CallStaticVoidMethod(activityClass, method, directory);
                requested = !env->ExceptionCheck();
                env->DeleteLocalRef(directory);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }

    if (attached) vm->DetachCurrentThread();
    return requested ? 1 : 0;
}

static void updateWindowRefreshRate(float targetFps) {
    static float sLastAppliedFps = -1.0f;
    if (fabsf(sLastAppliedFps - targetFps) < 0.5f) return;
    sLastAppliedFps = targetFps;

    if (!gMobile.android || !gMobile.android->activity || !gMobile.android->activity->vm || !gMobile.android->activity->clazz) return;
    JavaVM* vm = gMobile.android->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint getEnvRes = vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (getEnvRes == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
        attached = true;
    } else if (getEnvRes != JNI_OK || !env) {
        return;
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass clazz = env->GetObjectClass(gMobile.android->activity->clazz);
    if (clazz) {
        jmethodID mid = env->GetStaticMethodID(clazz, "setPreferredRefreshRate", "(F)V");
        if (mid) {
            env->CallStaticVoidMethod(clazz, mid, (jfloat)targetFps);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(clazz);
    }
    if (attached) vm->DetachCurrentThread();
}

static int64_t sLastPresentNs = 0;

// The game itself runs at the console's 59.94 Hz retrace; a refresh target
// above 60 can only repeat the last image. Repeating it re-runs FXAA, the UI
// and the buffer swap for no new information and costs heat and battery, so it
// is off. Set to true to get the old "repeat for high refresh" behaviour back.
static constexpr bool kRepeatGameFramesAboveNative = false;

static int64_t sLastNewGameFrameNs = 0;

// Free camera (Mods tab). Returns true and fills `out` when the frame should be
// drawn from the free camera instead of the game's own. Only used on the
// OpenGL ES path; with the switch off this does nothing and the game renders
// exactly as before.
static freecam::State sFreeCam;
static int64_t sFreeCamLastNs = 0;

static bool freeCameraStep(MobileApp& a, const gpu::CameraInfo& cam, gpu::EyeView& out) {
    const bool wanted = mobile_ui::get().settings().modFreeCamera;
    const int fovOffset = mobile_ui::get().settings().fovOffset;
    if (!wanted) {
        if (sFreeCam.active) {
            sFreeCam.active = false;
            port_input_block_game(0);
            LOGI("Free camera off");
        }
        if (fovOffset != 0 && cam.valid && mobile_ui::get().isGameplayActive()) {
            // FOV only: the game's own view, drawn with a wider / narrower lens.
            // With the offset at 0 none of this runs and the game renders as before.
            for (int i = 0; i < 16; i++) out.view[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            freecam::projection(cam.fovy + (float)fovOffset, cam.aspect, out.proj, 60.0f, 500000.0f);
            out.eyePos[0] = out.eyePos[1] = out.eyePos[2] = 0.0f;
            out.eyePos[3] = 1.0f;
            return true;
        }
        return false;
    }
    if (!cam.valid) return false;  // no 3D camera in this frame (menus, movies)

    const int64_t nowNs = port_host_time_ns();
    if (!sFreeCam.active) {
        freecam::begin(sFreeCam, cam.view);
        port_input_block_game(1);
        sFreeCamLastNs = nowNs;
        LOGI("Free camera on");
    }
    const float dt = sFreeCamLastNs ? (float)(nowNs - sFreeCamLastNs) * 1e-9f : 0.0f;
    sFreeCamLastNs = nowNs;

    const uint32_t buttons = combinedPadButtons(a);
    const float up = ((buttons & 0x0800u) ? 1.0f : 0.0f) - ((buttons & 0x0400u) ? 1.0f : 0.0f);  // A up, B down
    const bool fast = (buttons & 0x2000u) != 0;                                                    // Z
    freecam::step(sFreeCam, dt, a.pad.stickX, up, a.pad.stickY,
                  a.pad.rightStickX, a.pad.rightStickY, fast);

    freecam::eyeView(sFreeCam, cam.view, out.view);
    freecam::projection(cam.fovy + (float)fovOffset, cam.aspect, out.proj);
    float eyePos[3];
    freecam::positionInGameView(sFreeCam, cam.view, eyePos);
    out.eyePos[0] = eyePos[0];
    out.eyePos[1] = eyePos[1];
    out.eyePos[2] = eyePos[2];
    out.eyePos[3] = 1.0f;
    return true;
}

// Sleep used when no new game frame is ready. Right after a frame the next one
// is ~16 ms away, so a coarse sleep is safe; poll finely only while one is due.
static void idleWaitForGameFrame() {
    const int64_t sinceNewFrameNs = port_host_time_ns() - sLastNewGameFrameNs;
    const bool frameDueSoon = sinceNewFrameNs >= 11000000ll && sinceNewFrameNs < 100000000ll;
    usleep(frameDueSoon ? 1000 : 3000);
}

// Frame rate limiter and pacer.
// Enforces minimum frame spacing without burning CPU cycles in busy-wait loops.
static void syncMobileFrameRate(int targetFps, float speed) {
    if (targetFps < 30) targetFps = 30;
    if (targetFps > 144) targetFps = 144;
    static int sCurrentRefreshRate = 0;
    // A panel running above 60 Hz only helps when the game produces more than
    // 60 new frames a second (fast-forward); otherwise it just draws more power.
    // The caller passes 1.0 from UI-only redraws, so read the real game speed here.
    const int panelHz = (port_get_game_speed() > 1.05f || kRepeatGameFramesAboveNative)
        ? targetFps : std::min(targetFps, 60);
    if (sCurrentRefreshRate != panelHz) {
        sCurrentRefreshRate = panelHz;
        updateWindowRefreshRate((float)panelHz);
        port_set_display_refresh_rate(panelHz);
    }

    if (speed > 1.05f) {
        targetFps = (int)((float)targetFps * speed);
    }
    int64_t framePeriodNs = 1000000000ll / targetFps;

    int64_t nowNs = port_host_time_ns();
    if (sLastPresentNs != 0) {
        int64_t elapsedNs = nowNs - sLastPresentNs;
        if (elapsedNs < framePeriodNs) {
            int64_t waitNs = framePeriodNs - elapsedNs;
            // Leave a 1.2 ms margin so Android kernel timer slack does not cause a missed VSync pulse
            if (waitNs > 1500000ll) {
                struct timespec req;
                int64_t sleepNs = waitNs - 1200000ll;
                req.tv_sec = sleepNs / 1000000000ll;
                req.tv_nsec = sleepNs % 1000000000ll;
                int64_t sleepStartNs = port_host_time_ns();
                nanosleep(&req, nullptr);
                port_perf_pace(port_host_time_ns() - sleepStartNs);
            }
        }
    }

    sLastPresentNs = port_host_time_ns();
}

extern "C" __attribute__((visibility("default"))) void port_android_main(struct android_app* app, uintptr_t windowBase, size_t windowSize) {
    const char* privateDataPath =
        (app && app->activity && app->activity->internalDataPath)
            ? app->activity->internalDataPath : nullptr;
    char logPath[512] = "unavailable";
    if (privateDataPath && privateDataPath[0]) {
        const int pathLength = snprintf(logPath, sizeof(logPath), "%s/galaxy_log.txt", privateDataPath);
        if (pathLength > 0 && (size_t)pathLength < sizeof(logPath)) {
            port_log_file(logPath);
        } else {
            snprintf(logPath, sizeof(logPath), "unavailable (private path too long)");
        }
    }
    LOGI("Native engine entry: internal log %s",
         privateDataPath && privateDataPath[0] ? logPath : "unavailable");
    {
        char abi[PROP_VALUE_MAX] = {0}, hw[PROP_VALUE_MAX] = {0}, board[PROP_VALUE_MAX] = {0}, fp[PROP_VALUE_MAX] = {0};
        __system_property_get("ro.product.cpu.abilist", abi);
        __system_property_get("ro.hardware", hw);
        __system_property_get("ro.product.board", board);
        __system_property_get("ro.build.fingerprint", fp);
        long memKb = 0;
        if (FILE* mf = fopen("/proc/meminfo", "r")) {
            char l[128];
            if (fgets(l, sizeof(l), mf)) sscanf(l, "MemTotal: %ld kB", &memKb);
            fclose(mf);
        }
        LOGI("System: abi=%s hardware=%s board=%s cores=%ld/%ld ram=%ldMB pid=%d", abi[0] ? abi : "?", hw[0] ? hw : "?",
             board[0] ? board : "?", sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_NPROCESSORS_CONF), memKb / 1024,
             (int)getpid());
        LOGI("System: fingerprint=%s", fp[0] ? fp : "?");
    }
    sBootNs = port_host_time_ns();
    LOGI("Build: fork v37 (readable achievement notifications, RESET toggle next to the buttons; randomizer, seeds) compiled %s %s",
         __DATE__, __TIME__);

    MobileApp& a = gMobile;
    a.android = app;
    std::string internalPath = app->activity->internalDataPath ? app->activity->internalDataPath : "";
    bool isPlus = (internalPath.find("plus") != std::string::npos);
    std::string ext = app->activity->externalDataPath
        ? app->activity->externalDataPath
        : (isPlus ? "/sdcard/Android/data/com.galaxy.mobile.plus/files"
                  : "/sdcard/Android/data/com.galaxy.mobile/files");
    a.saveRoot = internalPath + "/nand";
    mkdir(a.saveRoot.c_str(), 0770);

    LOGI("==================================================");
    LOGI("GalaxyMobile: Native flat-screen mobile initialized");
    startFreezeWatchdog();
    LOGI("==================================================");
    LOGI("Native startup log opened at %s", logPath);

    char manufacturer[PROP_VALUE_MAX] = {};
    char model[PROP_VALUE_MAX] = {};
    char androidRelease[PROP_VALUE_MAX] = {};
    char androidApi[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.manufacturer", manufacturer);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.version.release", androidRelease);
    __system_property_get("ro.build.version.sdk", androidApi);
    LOGI("Device: %s %s | Android %s (API %s)",
         manufacturer[0] ? manufacturer : "unknown",
         model[0] ? model : "unknown",
         androidRelease[0] ? androidRelease : "unknown",
         androidApi[0] ? androidApi : "unknown");
    app->userData = &a;
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInput;
    initSensors(app);

    port_mem_set_reserved_window(windowBase, windowSize);

    a.pad.connected = 1;
    port_input_set(0, &a.pad);
    // Hold-A skipping follows the "Saltar cinematicas" setting (off by default).
    port_skip_set_hold_seconds(mobile_ui::get().settings().modSkipCutscenes ? 1.0f : 0.0f);

    mobile_ui::get().setPlusEdition(isPlus);
    LOGI("GalaxyMobile edition detected: %s (internalDataPath: %s)", isPlus ? "GalaxyMobile+" : "GalaxyMobile (Standard)", internalPath.c_str());

    mobile_ui::get().setSavePath(a.saveRoot + (isPlus ? "/touch_settings_plus.ini" : "/touch_settings.ini"));
    mobile_ui::get().setAchievementsPath(internalPath + "/galaxy_achievements.ini");
    int initialFps = mobile_ui::get().settings().targetFps;
    if (mobile_ui::get().settings().batterySaver) initialFps = 30;
    updateWindowRefreshRate((float)initialFps);

    LOGI("Native diagnostics session opened; previous session tail is included when available");
    LOGI("Device diagnostics: %s %s | Android %s (API %s)",
         manufacturer[0] ? manufacturer : "unknown",
         model[0] ? model : "unknown",
         androidRelease[0] ? androidRelease : "unknown",
         androidApi[0] ? androidApi : "unknown");
    gpu::setShaderCachePath((std::string(app->activity->internalDataPath) + "/shaders.bin").c_str());
    gpu::setVkPipelineCachePath((std::string(app->activity->internalDataPath) + "/vk_pipeline_cache.bin").c_str());
    // Compiled SPIR-V, so shaderc only runs for a program once per install.
    gpu::setVulkanShaderCachePath((std::string(app->activity->internalDataPath) + "/vk_shaders.bin").c_str());

    // Search for game data files
    a.dataRoot = findGameData(ext);
    if (a.dataRoot.empty()) {
        promptStoragePermissionIfMissing(app->activity);
    }

    bool running = true;
    while (running && !app->destroyRequested) {
        int events;
        android_poll_source* source = nullptr;
        int timeout = (a.windowReady) ? 0 : 50;
        int ident;
        while ((ident = ALooper_pollOnce(timeout, nullptr, &events, (void**)&source)) >= 0) {
            if (ident == 3) {
                pollSensors(a);
            }
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                running = false;
                break;
            }
            timeout = 0;
        }

        if (!a.windowReady || port_is_paused()) {
            usleep(25000);
            continue;
        }

        pollSensors(a);

        // Boot game once window and data are available
        if (!a.gameBooted) {
            if (!a.dataRoot.empty()) {
                // Language selection: user manual preference first, then fallback to device locale
                int userLang = mobile_ui::UIOverlay::get().settings().languageChoice;
                if (userLang == 1) port_language_set("en");
                else if (userLang == 2) port_language_set("es");
                else if (userLang == 3) port_language_set("fr");
                else if (userLang == 4) port_language_set("de");
                else if (userLang == 5) port_language_set("it");
                else {
                    char sysLocale[PROP_VALUE_MAX] = {0};
                    if (__system_property_get("persist.sys.locale", sysLocale) > 0 ||
                        __system_property_get("ro.product.locale", sysLocale) > 0) {
                        char langCode[3] = { (char)tolower(sysLocale[0]), (char)tolower(sysLocale[1]), 0 };
                        port_language_set(langCode);
                        LOGI("Auto-detected device language: %s -> set language code: %s", sysLocale, langCode);
                    }
                }

                LOGI("Booting game from: %s", a.dataRoot.c_str());
                a.gameBooted = true;
                port_boot(a.dataRoot.c_str(), a.saveRoot.c_str());
            } else {
                static int64_t lastLog = 0;
                static int missingCount = 0;
                int64_t now = port_host_time_ns();
                if (now - lastLog > 2000000000ll) {  // Every 2 seconds
                    lastLog = now;
                    missingCount++;
                    LOGI("Waiting for game files in /sdcard/GalaxyData or %s/game (attempt %d)", ext.c_str(), missingCount);
                    // Re-scan in case player pushed files
                    a.dataRoot = findGameData(ext);
                    if (a.dataRoot.empty() && missingCount >= 2) {
                        LOGE("Game data missing or incomplete, redirecting to launcher activity");
                        port_android_exit_to_launcher();
                    }
                }
            }
        }

        refreshGamepadConnection(a);

        // Update virtual pad pulse timers, auto-release, and hardware stats
        mobile_ui::get().update(a.pad, a.spinFrames);

        // Handle spin shake decay and feed input state
        if (a.spinFrames > 0) {
            float s = (a.spinFrames & 1) ? 2.5f : -2.5f;
            a.pad.accX = s;
            a.pad.accY = s;
            a.spinFrames--;
        } else {
            a.pad.accX = 0.0f;
            a.pad.accY = 0.0f;
        }
        if (mobile_ui::get().isStickActive()) {
            a.pad.stickX = mobile_ui::get().getTouchStickX();
            a.pad.stickY = mobile_ui::get().getTouchStickY();
        } else if (a.gamepadDetected && (fabsf(a.physicalStickX) > 0.001f || fabsf(a.physicalStickY) > 0.001f)) {
            a.pad.stickX = a.physicalStickX;
            a.pad.stickY = a.physicalStickY;
        } else {
            a.pad.stickX = 0.0f;
            a.pad.stickY = 0.0f;
        }

        // "Reset camera" button: the game's manual orbit and the free camera go
        // back to the game's own pose.
        if (sCameraResetRequested.exchange(false)) {
            port_camera_reset();
            sFreeCam.active = false;  // the free camera restarts from the game's view
            sGyroAccYaw = sGyroAccPitch = sGyroAccDt = 0.0;
            sGyroStickX = sGyroStickY = 0.0f;
            LOGI("camera: reset (orbit and free camera back to the game's own pose)");
        }

        // Second on-screen joystick + phone motion drive the camera (right stick).
        // Only touches the right stick while one of them is active, and hands it
        // back to a physical controller untouched.
        {
            const auto& cs = mobile_ui::get().settings();
            float cx = 0.0f, cy = 0.0f;
            bool inject = false;
            if (cs.showCamStick && !a.gamepadDetected && mobile_ui::get().getCamStickX() != 0.0f) { inject = true; }
            if (cs.showCamStick && !a.gamepadDetected && mobile_ui::get().getCamStickY() != 0.0f) { inject = true; }
            if (inject) {
                cx = mobile_ui::get().getCamStickX();
                cy = mobile_ui::get().getCamStickY();
            }
            if (cs.gyroCamera && !port_is_pointer_menu_navigation()) {
                const float camSens = std::max(0.25f, std::min(2.0f, port_input_camera_sensitivity()));
                if (sGyroAccDt > 0.003) {
                    const float yawRate = (float)(sGyroAccYaw / sGyroAccDt);
                    const float pitchRate = (float)(sGyroAccPitch / sGyroAccDt);
                    sGyroAccYaw = sGyroAccPitch = sGyroAccDt = 0.0;
                    const float gain = cs.gyroCameraSens;  // 1.0 = the camera turns as much as the phone
                    const float tx = gyroToStick(yawRate * gain, 1.20f * camSens);
                    const float ty = gyroToStick(pitchRate * gain, 0.90f * camSens);
                    sGyroStickX += (tx - sGyroStickX) * 0.6f;  // light smoothing of sensor noise
                    sGyroStickY += (ty - sGyroStickY) * 0.6f;
                } else if (port_host_time_ns() - sGyroCamNs > 120000000ll) {
                    sGyroStickX = sGyroStickY = 0.0f;  // the sensor went quiet: stop turning
                }
                if (fabsf(sGyroStickX) > 0.001f || fabsf(sGyroStickY) > 0.001f) {
                    cx += sGyroStickX;
                    cy += sGyroStickY;
                    inject = true;
                }
            } else {
                sGyroAccYaw = sGyroAccPitch = sGyroAccDt = 0.0;
                sGyroStickX = sGyroStickY = 0.0f;
            }
            if (inject) {
                a.pad.rightStickX = std::max(-1.0f, std::min(1.0f, cx));
                a.pad.rightStickY = std::max(-1.0f, std::min(1.0f, cy));
                sCamInjected = true;
            } else if (sCamInjected) {
                sCamInjected = false;
                if (!a.gamepadDetected) {
                    a.pad.rightStickX = 0.0f;
                    a.pad.rightStickY = 0.0f;
                }
            }
        }

        // In pointer-driven menus, the left stick and D-pad move the native
        // Star Pointer. The right stick remains camera-only everywhere.
        const int64_t pointerNowNs = port_host_time_ns();
        if (!port_is_pointer_menu_navigation()) {
            a.pointerNavLastNs = pointerNowNs;
            a.pointerNavLogged = false;
        } else {
            float navX = a.pad.stickX;
            float navY = a.pad.stickY;
            const uint32_t dpad = a.physicalButtons | a.physicalAxisButtons;
            const float dpadX = ((dpad & 0x0002) ? 1.0f : 0.0f) -
                                ((dpad & 0x0001) ? 1.0f : 0.0f);
            const float dpadY = ((dpad & 0x0008) ? 1.0f : 0.0f) -
                                ((dpad & 0x0004) ? 1.0f : 0.0f);
            navX = std::max(-1.0f, std::min(1.0f, navX + dpadX));
            navY = std::max(-1.0f, std::min(1.0f, navY + dpadY));
            const char* navSource = mobile_ui::get().isStickActive()
                                        ? "touch-left"
                                        : ((dpadX != 0.0f || dpadY != 0.0f)
                                               ? "dpad"
                                               : "left");
            if (a.gamepadDetected && !a.pad.pointerValid) {
                a.pad.pointerX = 0.0f;
                a.pad.pointerY = 0.0f;
                a.pad.pointerDist = 2.0f;
                a.pad.pointerValid = 1;
                mobile_ui::get().setPointerPos(0.0f, 0.0f);
                LOGI("controller: initialized menu pointer at screen center");
            }
            if (a.pointerNavLastNs == 0) {
                a.pointerNavLastNs = pointerNowNs;
            }
            const float dt = std::max(0.0f, std::min(0.05f,
                (float)(pointerNowNs - a.pointerNavLastNs) * 1.0e-9f));
            a.pointerNavLastNs = pointerNowNs;
            if (fabsf(navX) >= 0.08f || fabsf(navY) >= 0.08f) {
                constexpr float kMenuPointerSpeed = 1.6f;
                a.pad.pointerX = std::max(-1.0f, std::min(1.0f,
                    a.pad.pointerX + navX * kMenuPointerSpeed * dt));
                a.pad.pointerY = std::max(-1.0f, std::min(1.0f,
                    a.pad.pointerY - navY * kMenuPointerSpeed * dt));
                a.pad.pointerValid = 1;
                a.pad.pointerDist = 2.0f;
                // UIOverlay::update writes its stored pointer to PortPadState
                // every frame; keep that source synchronized or it snaps back
                // to the old touch position on the next frame.
                mobile_ui::get().setPointerPos(a.pad.pointerX, a.pad.pointerY);
                a.pad.pointerX = mobile_ui::get().getPointerX();
                a.pad.pointerY = mobile_ui::get().getPointerY();
                if (!a.pointerNavLogged) {
                    LOGI("controller: menu Star Pointer navigation active via %s stick", navSource);
                    a.pointerNavLogged = true;
                }
            }
        }
        a.pad.buttons = combinedPadButtons(a);
        port_input_set(0, &a.pad);

        // Direct hardware haptics integration: poll game engine rumble
        if (mobile_ui::get().settings().haptics) {
            static int sLastRumbleState = 0;
            int r = port_input_rumble(0);
            if (r && !sLastRumbleState) {
                port_android_vibrate(45);
            }
            sLastRumbleState = r;
        }


        // A Vulkan device can be lost mid-session (driver reset, GPU fault).
        // Vulkan expects the application to create a new device and carry on, so
        // that is what is tried first: dropping the rest of the session onto
        // OpenGL ES changes both the picture and the frame rate, and the player
        // notices.  Only a Vulkan that cannot be recreated falls back - or one
        // that is lost again straight away, since recreating it in a loop tears
        // the swapchain down over and over, which the player sees as the screen
        // flickering, and never reaches a playable state.
        if (a.vkReady && gpu::VkRenderer::get().hasFailed()) {
            static int64_t sLastRecreateNs = 0;
            static int sQuickRecreates = 0;
            int64_t nowNs = port_host_time_ns();
            sQuickRecreates = (sLastRecreateNs && nowNs - sLastRecreateNs < 15000000000ll) ? (sQuickRecreates + 1) : 0;
            sLastRecreateNs = nowNs;

            LOGW("Vulkan renderer failed at runtime; recreating the Vulkan device (attempt %d)", sQuickRecreates + 1);
            if (a.vkTarget.framebuffer) {
                gpu::VkRenderer::get().destroyTarget(a.vkTarget);
            }
            a.vkTarget = gpu::VkTarget{};
            a.targetReady = false;
            gpu::VkRenderer::get().shutdown();
            a.vkReady = false;
            // The shader and pipeline caches on disk mean the recreation below does
            // not pay for the programs again.
            if (sQuickRecreates < 2 && bringUpVulkan(a, a.android ? a.android->window : nullptr)) {
                LOGI("Vulkan device recreated; the session continues on Vulkan");
                continue;
            }
            LOGE("Vulkan was lost again immediately (or could not be recreated); falling back to OpenGL ES 3.2");
            sQuickRecreates = 0;
            if (initEgl(a) && createSurface(a)) {
                mobile_ui::get().setActiveBackend(0);
                LOGI("OpenGL ES 3.2 fallback renderer active");
            } else {
                LOGE("OpenGL ES fallback failed too; no rendering backend is available");
            }
            continue;
        }

        // Presentation & Render Loop
        if (a.vkReady) {
            gpu::VkRenderer& vkr = gpu::VkRenderer::get();
            int64_t updateStartNs = port_host_time_ns();
            bool newFrame = vkr.update();
            port_perf_update(port_host_time_ns() - updateStartNs);

            if (newFrame) {
                gPresentedFrames.store(vkr.frameNumber());
                sLastNewGameFrameNs = port_host_time_ns();
                mobile_ui::get().reportGameFramePresented();

                const mobile_ui::Settings& settings = mobile_ui::get().settings();
                const int scaleChoice = effectiveRenderScale(settings);
                const RenderTargetSpec requested =
                    renderTargetSpec(a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice);
                if (targetRequestChanged(a, scaleChoice, requested)) {
                    int actualScale = -1;
                    int targetW = 0, targetH = 0;
                    gpu::VkTarget newTarget = createTargetWithFallback<gpu::VkTarget>(
                        a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice, "Vulkan",
                        actualScale, targetW, targetH,
                        [&vkr](int width, int height) { return vkr.createTarget(width, height); },
                        [](const gpu::VkTarget& target) { return target.framebuffer != VK_NULL_HANDLE; });
                    rememberTargetRequest(a, scaleChoice, requested);
                    if (newTarget.framebuffer != VK_NULL_HANDLE) {
                        if (a.vkTarget.framebuffer) vkr.destroyTarget(a.vkTarget);
                        a.vkTarget = newTarget;
                        a.targetReady = true;
                        a.activeRenderScale = actualScale;
                        a.lastCompleteFrameValid = false;
                        updateViewport(a);
                        LOGI("Resized Vulkan target: %dx%d quality=%d requested=%d",
                             targetW, targetH, actualScale, scaleChoice);
                    } else {
                        LOGE("Vulkan quality change failed; keeping previous target %dx%d",
                             a.vkTarget.width, a.vkTarget.height);
                    }
                }
                if (!a.targetReady) continue;

                int64_t renderStartNs = port_host_time_ns();
                // Render into offscreen target
                vkr.render(a.vkTarget, nullptr, nullptr, gpu::HudMode::Inline);
                int64_t renderEndNs = port_host_time_ns();
                port_perf_render(renderEndNs - renderStartNs, 0);
                mobile_ui::get().reportGpuRenderTime(renderEndNs - renderStartNs);

                // Present to swapchain with pillarboxing
                updateViewport(a);
                int64_t presentStartNs = port_host_time_ns();
                vkr.present(a.vkTarget, a.vpX, a.vpY, a.vpW, a.vpH,
                            mobile_ui::get().settings().antialiasingLevel);
                port_perf_present(port_host_time_ns() - presentStartNs);
                mobile_ui::get().reportFramePresented();

                // Frame pacing: enforce minimum time spacing between presents so FPS NEVER exceeds targetFps
                int targetFps = mobile_ui::get().settings().targetFps;
                if (mobile_ui::get().settings().batterySaver) targetFps = 30;
                syncMobileFrameRate(targetFps, port_get_game_speed());
            } else {
                int targetFps = mobile_ui::get().settings().targetFps;
                if (mobile_ui::get().settings().batterySaver) targetFps = 30;
                float speed = port_get_game_speed();
                int effectiveFps = (speed > 1.05f) ? (int)((float)targetFps * speed) : targetFps;
                int64_t framePeriodNs = 1000000000ll / (effectiveFps > 0 ? effectiveFps : 60);
                int64_t nowNs = port_host_time_ns();

                bool highRefreshSlot = (kRepeatGameFramesAboveNative && targetFps > 60 && a.targetReady && vkr.hasFrame() && (nowNs - sLastPresentNs >= framePeriodNs));
                bool uiRedrawNeeded = (mobile_ui::get().needsRedraw() && a.targetReady && vkr.hasFrame() &&
                                       (!a.gameBooted || port_is_paused() || mobile_ui::get().isSettingsOpen()));

                if (highRefreshSlot || uiRedrawNeeded) {
                    syncMobileFrameRate(targetFps, 1.0f);
                    updateViewport(a);
                    vkr.present(a.vkTarget, a.vpX, a.vpY, a.vpW, a.vpH,
                                mobile_ui::get().settings().antialiasingLevel);
                    mobile_ui::get().reportFramePresented();
                } else {
                    idleWaitForGameFrame();
                }
            }
        } else {
            // OpenGL ES fallback loop
            gpu::Renderer& r = gpu::renderer();
            bool newFrame = r.update();

            if (newFrame) {
                gPresentedFrames.store(r.frameNumber());
                sLastNewGameFrameNs = port_host_time_ns();
                adaptiveQualityOnGameFrame(sLastNewGameFrameNs);
                perfOnGameFrame(a, sLastNewGameFrameNs, adaptiveFxaaLevel(mobile_ui::get().settings().antialiasingLevel),
                                sAdaptiveCap);
                mobile_ui::get().reportGameFramePresented();
                a.loggedFrameHold = false;

                // Check if offscreen target dimensions need to change (e.g. toggled fullscreen or resolution scale)
                const mobile_ui::Settings& settings = mobile_ui::get().settings();
                const int scaleChoice = effectiveRenderScale(settings);
                const RenderTargetSpec requested =
                    renderTargetSpec(a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice);
                if (targetRequestChanged(a, scaleChoice, requested)) {
                    int actualScale = -1;
                    int targetW = 0, targetH = 0;
                    gpu::EfbTarget newTarget = createTargetWithFallback<gpu::EfbTarget>(
                        a.screenWidth, a.screenHeight, settings.fullscreen, scaleChoice, "GLES",
                        actualScale, targetW, targetH,
                        [&r](int width, int height) { return r.createTarget(width, height); },
                        [](const gpu::EfbTarget& target) { return target.fbo != 0; });
                    rememberTargetRequest(a, scaleChoice, requested);
                    if (newTarget.fbo != 0) {
                        if (a.efbTarget.fbo != 0) r.destroyTarget(a.efbTarget);
                        a.efbTarget = newTarget;
                        a.targetReady = true;
                        a.activeRenderScale = actualScale;
                        a.lastCompleteFrameValid = false;
                        a.loggedFrameHold = false;
                        updateViewport(a);
                        LOGI("Resized GLES target: %dx%d quality=%d requested=%d",
                             targetW, targetH, actualScale, scaleChoice);
                    } else {
                        LOGE("GLES quality change failed; keeping previous target %dx%d",
                             a.efbTarget.width, a.efbTarget.height);
                    }
                }
                if (!a.targetReady) continue;

                int64_t renderStartNs = port_host_time_ns();
                // Monoscopic rendering into offscreen target (from the free
                // camera when it is on, otherwise exactly the game's view).
                gpu::EyeView freeEye;
                const bool useFreeCam = freeCameraStep(a, r.camera(), freeEye);
                sFovThroughCamera = useFreeCam;
                r.render(a.efbTarget, useFreeCam ? &freeEye : nullptr, nullptr, gpu::HudMode::Inline);

                // Clean GL states before clearing backbuffer and blitting
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, a.screenWidth, a.screenHeight);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                glDisable(GL_BLEND);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                // Clear black pillarbox borders
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                // FXAA is an optional post-process after the selected render
                // resolution. It leaves the target and quality preset untouched.
                drawGlesGameImage(a);
                a.lastCompleteFrameValid = a.targetReady;

                // Render on-screen controls HUD & Settings overlay
                mobile_ui::get().render(a.screenWidth, a.screenHeight);

                presentEglSurface(a);
                int64_t renderEndNs = port_host_time_ns();
                port_perf_render(renderEndNs - renderStartNs, 0);
                mobile_ui::get().reportGpuRenderTime(renderEndNs - renderStartNs);
                mobile_ui::get().reportFramePresented();

                // Frame pacing: enforce minimum time spacing between presents so FPS NEVER exceeds targetFps
                int targetFps = mobile_ui::get().settings().targetFps;
                if (mobile_ui::get().settings().batterySaver) targetFps = 30;
                syncMobileFrameRate(targetFps, port_get_game_speed());
            } else {
                if (r.hasFrame()) {
                    a.loggedFrameHold = false;
                } else if (a.targetReady && a.lastCompleteFrameValid && !a.loggedFrameHold) {
                    LOGW("Renderer has no prepared frame; retaining last complete EFB image");
                    a.loggedFrameHold = true;
                }
                int targetFps = mobile_ui::get().settings().targetFps;
                if (mobile_ui::get().settings().batterySaver) targetFps = 30;
                float speed = port_get_game_speed();
                int effectiveFps = (speed > 1.05f) ? (int)((float)targetFps * speed) : targetFps;
                int64_t framePeriodNs = 1000000000ll / (effectiveFps > 0 ? effectiveFps : 60);
                int64_t nowNs = port_host_time_ns();

                bool highRefreshSlot = (kRepeatGameFramesAboveNative && targetFps > 60 && a.targetReady && r.hasFrame() && (nowNs - sLastPresentNs >= framePeriodNs));
                bool uiRedrawNeeded = (mobile_ui::get().needsRedraw() &&
                                       (!a.gameBooted || port_is_paused() || mobile_ui::get().isSettingsOpen()));

                if (highRefreshSlot || uiRedrawNeeded) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glViewport(0, 0, a.screenWidth, a.screenHeight);
                    glDisable(GL_SCISSOR_TEST);
                    glDisable(GL_DEPTH_TEST);
                    glDisable(GL_CULL_FACE);
                    glDisable(GL_BLEND);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                    // Same background as the idle branch below: before the first
                    // complete game frame the menu sits on the deep blue, so a UI
                    // redraw (drawer animation, touch) must not flash it black.
                    const bool haveGameFrame = a.targetReady && a.lastCompleteFrameValid;
                    if (haveGameFrame) {
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    } else {
                        glClearColor(0.04f, 0.05f, 0.10f, 1.0f);
                    }
                    glClear(GL_COLOR_BUFFER_BIT);

                    if (haveGameFrame) {
                        drawGlesGameImage(a);
                    }

                    syncMobileFrameRate(targetFps, 1.0f);

                    mobile_ui::get().render(a.screenWidth, a.screenHeight);
                    presentEglSurface(a);
                    mobile_ui::get().reportFramePresented();
                } else if (!r.hasFrame()) {
                    // Keep a complete game frame on screen if the renderer briefly
                    // has no prepared frame (surface/config churn). Use the idle
                    // background only before the first complete game frame exists.
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glViewport(0, 0, a.screenWidth, a.screenHeight);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    if (a.targetReady && a.lastCompleteFrameValid) {
                        drawGlesGameImage(a);
                    } else {
                        glClearColor(0.04f, 0.05f, 0.10f, 1.0f);  // Galaxy deep blue before first game frame
                        glClear(GL_COLOR_BUFFER_BIT);
                    }
                    mobile_ui::get().render(a.screenWidth, a.screenHeight);
                    presentEglSurface(a);
                    usleep(16000);
                } else {
                    idleWaitForGameFrame();
                }
            }
    }
}

    LOGI("GalaxyMobile exiting cleanly");
    destroySurface(a);
    _exit(0);
}

// ---- Mods log ---------------------------------------------------------------------
// Every mod the game asks about goes through modProbe(): the first answer, every change of the
// answer, and (per scene) how many times the game asked and how many times the answer was "on".
static const char* const kModNames[] = {
    "Luigi (play as Luigi)",
    "Fly mode",
    "Skip cutscenes",
    "Infinite health",
    "Infinite lives",
    "Infinite star bits",
    "Shield HD textures",
    "Shield 60 FPS cutscenes",
    "Shield controller curves",
    "ABXY gamepad button sheets",
    "Riivolution patches",
    "Difficulty: one-hit KO",
    "Difficulty: no healing",
    "Difficulty: max health 3",
    "Difficulty: enemy damage multiplier",
    "Difficulty: boss damage multiplier"};
static const int kModCount = (int)(sizeof(kModNames) / sizeof(kModNames[0]));
static std::atomic<int> sModLast[32];
static std::atomic<unsigned> sModAsked[32];
static std::atomic<unsigned> sModOn[32];
static std::atomic<unsigned> sModAskedTotal[32];
static std::atomic<unsigned> sModOnTotal[32];
static std::atomic<bool> sModSeen[32];

static int modProbe(int idx, int value) {
    if (idx >= 0 && idx < kModCount) {
        sModAsked[idx]++;
        sModAskedTotal[idx]++;
        if (value > 1 || value == 1) {
            sModOn[idx]++;
            sModOnTotal[idx]++;
        }
        const int prev = sModLast[idx].exchange(value);
        if (!sModSeen[idx].exchange(true)) {
            LOGI("mods: [%02d] %s: first asked by the game, answer %d (%s)", idx, kModNames[idx], value, value > 0 ? "ON" : "off");
        } else if (prev != value) {
            LOGI("mods: [%02d] %s: answer changed %d -> %d (%s)", idx, kModNames[idx], prev, value, value > 0 ? "ON" : "off");
        }
    }
    return value;
}

static void modsLogSceneSummary() {
    bool any = false;
    for (int i = 0; i < kModCount; i++) {
        const unsigned asked = sModAsked[i].exchange(0);
        const unsigned on = sModOn[i].exchange(0);
        if (asked == 0) continue;
        if (!any) {
            LOGI("mods: ---- use in the scene that just ended ----");
            any = true;
        }
        LOGI("mods: [%02d] %s: asked %u times, answered ON %u times, off %u times (since start: asked %u, ON %u)", i, kModNames[i], asked, on,
             asked - on, sModAskedTotal[i].load(), sModOnTotal[i].load());
    }
    if (any) LOGI("mods: ---- end of use ----");
}

extern "C" {
float port_get_display_aspect(void) {
    if (gMobile.screenHeight > 0) {
        return (float)gMobile.screenWidth / (float)gMobile.screenHeight;
    }
    return 16.0f / 9.0f;
}

int port_is_fullscreen_aspect(void) {
    return mobile_ui::UIOverlay::get().settings().fullscreen ? 1 : 0;
}

int port_is_force_luigi(void) {
    return modProbe(0, mobile_ui::UIOverlay::get().settings().modLuigi ? 1 : 0);
}

int port_is_fly_mode(void) {
    return modProbe(1, mobile_ui::UIOverlay::get().settings().modFly ? 1 : 0);
}

int port_is_skip_cutscenes(void) {
    return modProbe(2, mobile_ui::UIOverlay::get().settings().modSkipCutscenes ? 1 : 0);
}

int port_is_infinite_health(void) {
    return modProbe(3, mobile_ui::UIOverlay::get().settings().modInfiniteHealth ? 1 : 0);
}

int port_difficulty_enemy_damage(void) {
    return modProbe(14, std::max(1, std::min(4, mobile_ui::UIOverlay::get().settings().diffEnemyDamage)));
}

int port_difficulty_boss_damage(void) {
    return modProbe(15, std::max(1, std::min(4, mobile_ui::UIOverlay::get().settings().diffBossDamage)));
}

int port_difficulty_one_hit(void) {
    return modProbe(11, mobile_ui::UIOverlay::get().settings().diffOneHit ? 1 : 0);
}

int port_difficulty_no_heal(void) {
    return modProbe(12, mobile_ui::UIOverlay::get().settings().diffNoHeal ? 1 : 0);
}

int port_difficulty_max_health_3(void) {
    return modProbe(13, mobile_ui::UIOverlay::get().settings().diffMaxHealth3 ? 1 : 0);
}

// ---- Randomizer ------------------------------------------------------------------
static unsigned randoFlagsFromSettings(const mobile_ui::Settings& st) {
    unsigned flags = 0;
    if (st.randoEnabled) {
        if (st.randoGalaxies) flags |= rando::F_GALAXIES;
        if (st.randoStars) flags |= rando::F_STARS;
        if (st.randoEnemies) flags |= rando::F_ENEMIES;
        if (st.randoItems) flags |= rando::F_ITEMS;
        if (st.randoPowerUps) flags |= rando::F_POWERUPS;
        if (st.randoChallenge) flags |= rando::F_CHALLENGE;
    }
    return flags;
}

static unsigned randoSeedNumber(const mobile_ui::Settings& st) {
    return (unsigned)std::max(0, std::min(999999, st.randoSeed));
}

// ---- Randomizer log -------------------------------------------------------------
// Everything the randomizer decides is written to the log, in detail: the settings it reads, the
// full tables when they change, every door / mission / object decision and, at each scene change,
// a summary of the scene that just loaded.
static std::string randoFlagsText(unsigned f) {
    std::string t;
    auto add = [&](unsigned bit, const char* n) {
        t += n;
        t += (f & bit) ? "=on " : "=off ";
    };
    add(rando::F_GALAXIES, "galaxies");
    add(rando::F_STARS, "stars");
    add(rando::F_ENEMIES, "enemies");
    add(rando::F_ITEMS, "items");
    add(rando::F_POWERUPS, "powerups");
    add(rando::F_CHALLENGE, "challenge");
    return t;
}

static std::mutex sRandoLogLock;
struct RandoSceneStats {
    unsigned checked = 0, replaced = 0, logged = 0;
    // Host-heap containers: this struct is filled from emulated game threads and outlives every
    // scene, so it must not be allocated inside the game's JKRHeap (see port/randomizer.h).
    rando::HMap<rando::HString, unsigned> byName;  // "A -> B" -> times
};
static RandoSceneStats sRandoScene;
static unsigned sRandoSceneNo = 0;
static std::atomic<unsigned> sRandoDecisions{0};

static void randoLogTables(rando::Engine& e, unsigned seed, unsigned flags, const char* who) {
    LOGI("randomizer[%s]: ---- configuration ----", who);
    LOGI("randomizer[%s]: seed %06u (engine seed 0x%016llx)", who, seed, (unsigned long long)rando::seedFromNumber(seed));
    LOGI("randomizer[%s]: flags 0x%02x: %s", who, flags, randoFlagsText(flags).c_str());
    if (flags & rando::F_CHALLENGE) LOGI("randomizer[%s]: challenge mode: power-ups are never swapped", who);
    if (flags == 0) LOGI("randomizer[%s]: nothing is randomized with these settings", who);
    const auto lines = e.describe();
    LOGI("randomizer[%s]: %zu replacement entries in the tables", who, lines.size());
    for (size_t i = 0; i < lines.size(); i++) LOGI("randomizer[%s]:   [%02zu] %s", who, i + 1, lines[i].c_str());
    LOGI("randomizer[%s]: ---- end of configuration ----", who);
}

// Galaxy doors and missions are decided by one call each, so they follow the live settings.
static rando::Engine& randoEngine() {
    static rando::Engine engine;
    static bool logged = false;
    static unsigned lastSeed = 0, lastFlags = 0;
    const auto& st = mobile_ui::UIOverlay::get().settings();
    const unsigned seed = randoSeedNumber(st);
    const unsigned flags = randoFlagsFromSettings(st);
    engine.configure(rando::seedFromNumber(seed), flags);
    std::lock_guard<std::mutex> lock(sRandoLogLock);
    if (!logged || seed != lastSeed || flags != lastFlags) {
        if (logged) LOGI("randomizer[doors]: settings changed: seed %06u -> %06u, flags 0x%02x -> 0x%02x", lastSeed, seed, lastFlags, flags);
        logged = true;
        lastSeed = seed;
        lastFlags = flags;
        randoLogTables(engine, seed, flags, "doors");
    }
    return engine;
}

// Objects are mounted and created in two steps while a scene loads, so they use a snapshot taken
// when the scene change starts: changing the settings in the middle of a load cannot mix two
// configurations (which would mount one object's files and create another).
static std::mutex sRandoCommitLock;
static bool sRandoCommitted = false;
static unsigned sRandoCommitSeed = 0;
static unsigned sRandoCommitFlags = 0;

static rando::Engine& randoObjectEngine() {
    static rando::Engine engine;
    static bool logged = false;
    static unsigned lastSeed = 0, lastFlags = 0;
    std::lock_guard<std::mutex> lock(sRandoCommitLock);
    if (!sRandoCommitted) {
        const auto& st = mobile_ui::UIOverlay::get().settings();
        sRandoCommitSeed = randoSeedNumber(st);
        sRandoCommitFlags = randoFlagsFromSettings(st);
        sRandoCommitted = true;
        LOGI("randomizer[objects]: no snapshot yet, taking one now (seed %06u, flags 0x%02x)", sRandoCommitSeed, sRandoCommitFlags);
    }
    engine.configure(rando::seedFromNumber(sRandoCommitSeed), sRandoCommitFlags);
    if (!logged || sRandoCommitSeed != lastSeed || sRandoCommitFlags != lastFlags) {
        logged = true;
        lastSeed = sRandoCommitSeed;
        lastFlags = sRandoCommitFlags;
        randoLogTables(engine, sRandoCommitSeed, sRandoCommitFlags, "objects");
    }
    return engine;
}

static void randoLogSceneSummary() {
    std::lock_guard<std::mutex> lock(sRandoLogLock);
    if (sRandoScene.checked == 0 && sRandoScene.replaced == 0) return;
    LOGI("randomizer[scene %u]: summary: %u object names checked, %u replaced, %u replacement lines logged", sRandoSceneNo,
         sRandoScene.checked, sRandoScene.replaced, sRandoScene.logged);
    for (const auto& kv : sRandoScene.byName) {
        LOGI("randomizer[scene %u]:   %s x%u", sRandoSceneNo, kv.first.c_str(), kv.second);
    }
    sRandoScene = RandoSceneStats();
}

void port_rando_commit(void) {
    modsLogSceneSummary();
    randoLogSceneSummary();
    const auto& st = mobile_ui::UIOverlay::get().settings();
    unsigned seed, flags;
    {
        std::lock_guard<std::mutex> lock(sRandoCommitLock);
        seed = randoSeedNumber(st);
        flags = randoFlagsFromSettings(st);
        if (!sRandoCommitted || seed != sRandoCommitSeed || flags != sRandoCommitFlags) {
            LOGI("randomizer[commit]: objects for the next scene change from seed %06u/0x%02x to seed %06u/0x%02x", sRandoCommitSeed,
                 sRandoCommitFlags, seed, flags);
        } else {
            LOGI("randomizer[commit]: objects for the next scene keep seed %06u, flags 0x%02x (%s)", seed, flags, randoFlagsText(flags).c_str());
        }
        sRandoCommitSeed = seed;
        sRandoCommitFlags = flags;
        sRandoCommitted = true;
    }
    std::lock_guard<std::mutex> lock(sRandoLogLock);
    sRandoSceneNo++;
}

int port_rando_active(void) {
    return randoEngine().active() ? 1 : 0;
}

const char* port_rando_galaxy(const char* stage) {
    const char* out = randoEngine().galaxy(stage);
    const int n = (int)(++sRandoDecisions);
    if (out && stage && strcmp(out, stage) != 0) {
        LOGI("randomizer[door #%d]: %s -> %s (seed %06d): the galaxy door loads %s", n, stage, out,
             mobile_ui::UIOverlay::get().settings().randoSeed, out);
    } else {
        LOGI("randomizer[door #%d]: %s is not changed (not in the pool, or galaxies are off)", n, stage ? stage : "(null)");
    }
    return out;
}

int port_rando_scenario(const char* stage, int selected, int normalCount) {
    auto& e = randoEngine();
    const int out = e.scenario(stage, selected, normalCount);
    const int n = (int)(++sRandoDecisions);
    if (out != selected) {
        std::string perm;
        for (int i = 1; i <= normalCount; i++) perm += std::to_string(i) + "->" + std::to_string(e.scenario(stage, i, normalCount)) + " ";
        LOGI("randomizer[mission #%d]: %s: picked mission %d loads mission %d (%d normal missions; full table: %s)", n,
             stage ? stage : "?", selected, out, normalCount, perm.c_str());
    } else {
        LOGI("randomizer[mission #%d]: %s: picked mission %d is not changed (%d normal missions; stars %s, in pool %d)", n,
             stage ? stage : "?", selected, normalCount, (e.flags() & rando::F_STARS) ? "on" : "off", rando::inGalaxyPool(stage) ? 1 : 0);
    }
    return out;
}

const char* port_rando_object(const char* name) {
    const char* out = randoObjectEngine().object(name);
    std::lock_guard<std::mutex> lock(sRandoLogLock);
    sRandoScene.checked++;
    if (out && name && strcmp(out, name) != 0) {
        sRandoScene.replaced++;
        rando::HString key(name);
        key += " -> ";
        key += out;
        sRandoScene.byName[key]++;
        if (sRandoScene.logged < 300) {
            sRandoScene.logged++;
            LOGI("randomizer[scene %u]: object #%u %s is replaced by %s", sRandoSceneNo, sRandoScene.replaced, name, out);
        }
    }
    return out;
}

int port_is_infinite_lives(void) {
    return modProbe(4, mobile_ui::UIOverlay::get().settings().modInfiniteLives ? 1 : 0);
}

int port_is_infinite_starbits(void) {
    return modProbe(5, mobile_ui::UIOverlay::get().settings().modInfiniteStarBits ? 1 : 0);
}

int port_is_shield_hd_textures(void) {
    return modProbe(6, mobile_ui::UIOverlay::get().settings().modShieldTextures ? 1 : 0);
}

int port_is_shield_cutscenes(void) {
    return modProbe(7, mobile_ui::UIOverlay::get().settings().modShieldCutscenes ? 1 : 0);
}

int port_is_shield_controls(void) {
    return modProbe(8, mobile_ui::UIOverlay::get().settings().modShieldControls ? 1 : 0);
}

int port_is_classic_gamepad_ui(void) {
    return modProbe(9, mobile_ui::UIOverlay::get().settings().modClassicGamepadUI ? 1 : 0);
}

int port_is_riivolution_mods(void) {
    return modProbe(10, mobile_ui::UIOverlay::get().settings().modRiivolution ? 1 : 0);
}
}

