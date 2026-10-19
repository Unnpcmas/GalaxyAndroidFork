#include "mobile_ui.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "port/port.h"
#include "port/input.h"
#include "xr/ui_font.h"

#define LOGI(...) port_log("INFO MobileUI: " __VA_ARGS__)
#define LOGE(...) port_log("ERROR MobileUI: " __VA_ARGS__)

namespace mobile_ui {

extern "C" void port_android_exit_to_launcher();
extern "C" int port_android_copy_to_clipboard(const char* bytes, int length);
extern "C" void port_set_game_speed(float speed);
extern "C" float port_get_game_speed(void);
extern "C" void port_language_set(const char* name);
extern "C" void port_set_display_refresh_rate(int hz);
extern "C" int port_get_display_refresh_rate(void);

inline void applyTargetFps(int fps) {
    if (fps < 30) fps = 30;
    if (fps > 144) fps = 144;
    port_set_display_refresh_rate(fps);
}

inline void applyGameSpeed(int speedIdx) {
    float speed = 1.0f;
    switch (speedIdx) {
        case 1: speed = 1.0f; break;
        case 2: speed = 1.5f; break;
        case 3: speed = 2.0f; break;
        case 4: speed = 3.0f; break;
        case 5: speed = 4.0f; break;
        default: speed = 1.0f; break;
    }
    port_set_game_speed(speed);
}

inline void applyLanguage(int langIdx) {
    switch (langIdx) {
        case 1: port_language_set("english"); break;
        case 2: port_language_set("spanish"); break;
        case 3: port_language_set("french"); break;
        case 4: port_language_set("german"); break;
        case 5: port_language_set("italian"); break;
        default: break;
    }
}

namespace {

inline int64_t currentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

const char* controllerActionName(int action) {
    static const char* names[] = {"A", "B", "C", "Z", "SPIN", "PAUSA"};
    return action >= 0 && action < 6 ? names[action] : "CONTROL";
}

int* controllerBinding(Settings& settings, int action) {
    switch (action) {
    case 0: return &settings.controllerKeyA;
    case 1: return &settings.controllerKeyB;
    case 2: return &settings.controllerKeyC;
    case 3: return &settings.controllerKeyZ;
    case 4: return &settings.controllerKeySpin;
    case 5: return &settings.controllerKeyPause;
    default: return nullptr;
    }
}

float settingsDrawerWidth(float screenW, float screenH, float uiScale) {
    // The old 390-unit cap made landscape settings look compressed. Give the
    // sheet enough room for labels and controls while retaining a responsive
    // limit on phones and portrait layouts.
    const float widthFraction = screenW >= screenH ? 0.48f : 0.88f;
    return std::min(screenW * widthFraction, 620.0f * uiScale);
}

std::string controllerKeyName(int keyCode) {
    if (keyCode < 0) return "AUTO";
    switch (keyCode) {
    case AKEYCODE_BUTTON_A: return "A";
    case AKEYCODE_BUTTON_B: return "B";
    case AKEYCODE_BUTTON_X: return "X";
    case AKEYCODE_BUTTON_Y: return "Y";
    case AKEYCODE_BUTTON_L1: return "L1";
    case AKEYCODE_BUTTON_R1: return "R1";
    case AKEYCODE_BUTTON_L2: return "L2";
    case AKEYCODE_BUTTON_R2: return "R2";
    case AKEYCODE_BUTTON_START: return "START";
    case AKEYCODE_BUTTON_SELECT: return "SELECT";
    case AKEYCODE_DPAD_UP: return "DPAD↑";
    case AKEYCODE_DPAD_DOWN: return "DPAD↓";
    case AKEYCODE_DPAD_LEFT: return "DPAD←";
    case AKEYCODE_DPAD_RIGHT: return "DPAD→";
    default: {
        char label[20];
        snprintf(label, sizeof(label), "KEY %d", keyCode);
        return label;
    }
    }
}


struct Color {
    float r, g, b, a;
};

inline Color rgba(float r, float g, float b, float a) {
    return {r, g, b, a};
}

inline Color hexColor(uint32_t hex, float a) {
    float r = ((hex >> 16) & 0xFF) / 255.0f;
    float g = ((hex >> 8) & 0xFF) / 255.0f;
    float b = (hex & 0xFF) / 255.0f;
    return {r, g, b, a};
}

int base64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

const char* kVS = R"(#version 300 es
precision mediump float;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aParams;

uniform mat4 uProj;

out vec2 vUv;
out vec4 vColor;
out vec4 vParams;

void main() {
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
    vUv = aUv;
    vColor = aColor;
    vParams = aParams;
}
)";

const char* kFS = R"(#version 300 es
precision mediump float;

in vec2 vUv;
in vec4 vColor;
in vec4 vParams;

uniform sampler2D uFontTex;

out vec4 FragColor;

void main() {
    float mode = vParams.x;
    float param = vParams.y;
    float alpha = 1.0;

    if (mode > 0.5 && mode < 1.5) {
        // Mode 1: Circle or Ring (vUv in -1..1)
        float d = length(vUv);
        float fw = fwidth(d);
        if (fw < 0.001) fw = 0.005;
        float outer = smoothstep(1.0, 1.0 - fw, d);
        if (param > 0.0) {
            float inner = smoothstep(param - fw, param, d);
            alpha = outer * inner;
        } else {
            alpha = outer;
        }
    } else if (mode > 1.5 && mode < 2.5) {
        // Mode 2: Font glyph texture: sample red channel
        alpha = texture(uFontTex, vUv).r;
    } else if (mode > 2.5 && mode < 3.5) {
        // Mode 3: Iconic Material 3 Settings Cogwheel
        float d = length(vUv);
        float fw = fwidth(d);
        if (fw < 0.001) fw = 0.005;
        float angle = atan(vUv.y, vUv.x) + param;
        float teeth = cos(8.0 * angle);
        float rMax = 0.74 + 0.22 * smoothstep(-0.25, 0.25, teeth);
        float outer = smoothstep(rMax, rMax - fw * 1.5, d);
        float inner = smoothstep(0.33 - fw * 1.5, 0.33, d);
        alpha = outer * inner;
    } else if (mode > 3.5 && mode < 4.5) {
        // Mode 4: Material 3 Close 'X' icon
        vec2 p = abs(vUv);
        float lineDist = abs(p.x - p.y) * 0.7071;
        float capDist = max(p.x, p.y);
        float fw = fwidth(lineDist);
        if (fw < 0.001) fw = 0.005;
        float line = smoothstep(0.16, 0.16 - fw, lineDist);
        float box = smoothstep(0.65, 0.65 - fw, capDist);
        alpha = line * box;
    } else if (mode > 4.5 && mode < 5.5) {
        // Mode 5: Spin Cyclone / Galaxy Swirl Glyph
        float d = length(vUv);
        float a = atan(vUv.y, vUv.x) + param;
        float aFold = mod(a, 3.14159265);
        float normAngle = clamp((aFold - 0.25) / 2.35, 0.0, 1.0);
        float arcR = mix(0.42, 0.78, normAngle);
        float dArc = abs(d - arcR);
        float thick = mix(0.06, 0.16, normAngle);
        float fw = fwidth(dArc);
        if (fw < 0.001) fw = 0.005;
        float arcAlpha = smoothstep(thick, thick - fw * 1.5, dArc);
        float angleMask = smoothstep(0.12, 0.30, aFold) * smoothstep(2.80, 2.60, aFold);
        alpha = arcAlpha * angleMask;
    } else if (mode > 5.5 && mode < 6.5) {
        // Mode 6: Modern Precision Chevron Arrow (rot in param)
        float ca = cos(param);
        float sa = sin(param);
        vec2 p = vec2(ca * vUv.x - sa * vUv.y, sa * vUv.x + ca * vUv.y);
        vec2 tip = vec2(0.30, 0.0);
        vec2 wing = vec2(-0.30, 0.52);
        vec2 seg = wing - tip;
        vec2 pt = vec2(p.x, abs(p.y)) - tip;
        float t = clamp(dot(pt, seg) / dot(seg, seg), 0.0, 1.0);
        float dLine = length(pt - seg * t);
        float fw = fwidth(dLine);
        if (fw < 0.001) fw = 0.005;
        float strokeW = 0.16;
        alpha = smoothstep(strokeW, strokeW - fw * 1.5, dLine);
    } else if (mode > 6.5 && mode < 7.5) {
        // Mode 7: Modern Minimalist Camera Glyph
        vec2 p = vUv;
        vec2 bBody = vec2(0.48, 0.32);
        vec2 pBody = p - vec2(0.0, 0.06);
        vec2 dBody = abs(pBody) - bBody;
        float distBody = length(max(dBody, 0.0)) + min(max(dBody.x, dBody.y), 0.0) - 0.10;

        vec2 bTop = vec2(0.18, 0.08);
        vec2 pTop = p - vec2(-0.16, -0.32);
        vec2 dTop = abs(pTop) - bTop;
        float distTop = length(max(dTop, 0.0)) + min(max(dTop.x, dTop.y), 0.0) - 0.05;

        float bodyUnion = min(distBody, distTop);
        float fw = fwidth(length(p));
        if (fw < 0.001) fw = 0.005;
        float fillBody = smoothstep(0.0, -fw * 1.5, bodyUnion);

        float dLens = length(p - vec2(0.0, 0.08));
        float lensHole = smoothstep(0.24 - fw, 0.24, dLens);
        float lensRing = smoothstep(0.24, 0.24 - fw, dLens) * smoothstep(0.15 - fw, 0.15, dLens);
        float lensDot = smoothstep(0.07, 0.07 - fw, dLens);

        float dFlash = length(p - vec2(0.34, -0.12));
        float flashDot = smoothstep(0.05, 0.05 - fw, dFlash);

        alpha = max(fillBody * lensHole * (1.0 - flashDot), max(lensRing, lensDot));
    } else {
        // Mode 0: Rounded rectangle with exact aspect ratio
        if (param > 0.0) {
            vec2 p = abs(vUv);
            float aspect = vParams.z;
            vec2 b = (vParams.w > 0.5) ? vec2(1.0 - param, aspect - param) : vec2(aspect - param, 1.0 - param);
            vec2 d = p - b;
            float dist = length(max(d, 0.0)) - param;
            float fw = fwidth(dist);
            if (fw < 0.001) fw = 0.005;
            alpha = smoothstep(0.0, -fw, dist);
        }
    }

    if (alpha <= 0.002) discard;
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

void Settings::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    bool hasGraphicsProfileVersion = false;
    bool hasDisplayFitVersion = false;
    int graphicsProfileVersion = 0;
    int displayFitVersion = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        float val = 0.0f;
        if (sscanf(line, "%63[^=]=%f", key, &val) == 2) {
            if (!strcmp(key, "show_controls")) showControls = (val > 0.5f);
            else if (!strcmp(key, "achievement_notifications")) achievementNotifications = (val > 0.5f);
            else if (!strcmp(key, "fullscreen")) fullscreen = (val > 0.5f);
            else if (!strcmp(key, "screen_scale_index")) screenScaleIndex = std::max(0, std::min(2, (int)val));
            else if (!strcmp(key, "display_fit_version")) {
                displayFitVersion = (int)val;
                hasDisplayFitVersion = true;
            }
            else if (!strcmp(key, "perf_hud")) perfHudMode = std::max(0, std::min(2, (int)val));
            else if (!strcmp(key, "opacity")) opacity = std::max(0.1f, std::min(1.0f, val));
            else if (!strcmp(key, "scale")) scale = std::max(0.6f, std::min(1.6f, val));
            else if (!strcmp(key, "dynamic_stick")) dynamicStick = (val > 0.5f);
            else if (!strcmp(key, "stick_offset_x")) stickOffsetX = val;
            else if (!strcmp(key, "stick_offset_y")) stickOffsetY = val;
            else if (!strcmp(key, "button_offset_x")) buttonOffsetX = val;
            else if (!strcmp(key, "button_offset_y")) buttonOffsetY = val;
            else if (!strcmp(key, "btn_a_offset_x")) btnAOffsetX = val;
            else if (!strcmp(key, "btn_a_offset_y")) btnAOffsetY = val;
            else if (!strcmp(key, "btn_b_offset_x")) btnBOffsetX = val;
            else if (!strcmp(key, "btn_b_offset_y")) btnBOffsetY = val;
            else if (!strcmp(key, "btn_z_offset_x")) btnZOffsetX = val;
            else if (!strcmp(key, "btn_z_offset_y")) btnZOffsetY = val;
            else if (!strcmp(key, "btn_spin_offset_x")) btnSpinOffsetX = val;
            else if (!strcmp(key, "btn_spin_offset_y")) btnSpinOffsetY = val;
            else if (!strcmp(key, "pause_offset_x")) pauseOffsetX = val;
            else if (!strcmp(key, "pause_offset_y")) pauseOffsetY = val;
            else if (!strcmp(key, "show_stick")) showStick = (val > 0.5f);
            else if (!strcmp(key, "show_button_a")) showButtonA = (val > 0.5f);
            else if (!strcmp(key, "show_button_b")) showButtonB = (val > 0.5f);
            else if (!strcmp(key, "show_button_z")) showButtonZ = (val > 0.5f);
            else if (!strcmp(key, "show_button_spin")) showButtonSpin = (val > 0.5f);
            else if (!strcmp(key, "show_pause_button")) showPauseButton = (val > 0.5f);
            else if (!strcmp(key, "show_dpad")) showDpad = (val > 0.5f);
            else if (!strcmp(key, "dpad_on_right")) dpadOnRight = (val > 0.5f);
            else if (!strcmp(key, "dpad_offset_x")) dpadOffsetX = val;
            else if (!strcmp(key, "dpad_offset_y")) dpadOffsetY = val;
            else if (!strcmp(key, "controller_key_a")) controllerKeyA = std::max(-1, std::min(511, (int)val));
            else if (!strcmp(key, "controller_key_b")) controllerKeyB = std::max(-1, std::min(511, (int)val));
            else if (!strcmp(key, "controller_key_c")) controllerKeyC = std::max(-1, std::min(511, (int)val));
            else if (!strcmp(key, "controller_key_z")) controllerKeyZ = std::max(-1, std::min(511, (int)val));
            else if (!strcmp(key, "controller_key_spin")) controllerKeySpin = std::max(-1, std::min(511, (int)val));
            else if (!strcmp(key, "controller_key_pause")) controllerKeyPause = std::max(-1, std::min(511, (int)val));
            else if (!strcmp(key, "controller_deadzone")) controllerDeadzone = std::max(0.05f, std::min(0.40f, val));
            else if (!strcmp(key, "left_stick_sensitivity")) leftStickSensitivity = std::max(0.25f, std::min(2.00f, val));
            else if (!strcmp(key, "right_stick_sensitivity")) rightStickSensitivity = std::max(0.25f, std::min(2.00f, val));
            else if (!strcmp(key, "camera_invert_x")) cameraInvertX = (val > 0.5f);
            else if (!strcmp(key, "camera_invert_y")) cameraInvertY = (val > 0.5f);
            else if (!strcmp(key, "mod_shield_textures")) modShieldTextures = (val > 0.5f);
            else if (!strcmp(key, "mod_shield_cutscenes")) modShieldCutscenes = (val > 0.5f);
            else if (!strcmp(key, "mod_shield_controls")) modShieldControls = (val > 0.5f);
            else if (!strcmp(key, "mod_classic_gamepad_ui")) modClassicGamepadUI = (val > 0.5f);
            else if (!strcmp(key, "mod_riivolution")) modRiivolution = (val > 0.5f);
            else if (!strcmp(key, "mod_fly")) modFly = (val > 0.5f);
            else if (!strcmp(key, "mod_skip_cutscenes")) modSkipCutscenes = (val > 0.5f);
            else if (!strcmp(key, "mod_infinite_health")) modInfiniteHealth = (val > 0.5f);
            else if (!strcmp(key, "mod_infinite_lives")) modInfiniteLives = (val > 0.5f);
            else if (!strcmp(key, "mod_infinite_starbits")) modInfiniteStarBits = (val > 0.5f);
            else if (!strcmp(key, "game_speed")) gameSpeed = std::max(1, std::min(5, (int)val));
            else if (!strcmp(key, "language_choice")) languageChoice = std::max(0, std::min(5, (int)val));
            else if (!strcmp(key, "render_scale")) renderScale = std::max(0, std::min(4, (int)val));
            else if (!strcmp(key, "target_fps")) {
                int v = (int)val;
                if (v >= 144) targetFps = 144;
                else if (v >= 120) targetFps = 120;
                else if (v >= 90) targetFps = 90;
                else if (v >= 60) targetFps = 60;
                else targetFps = 30;
            }
            else if (!strcmp(key, "adaptive_fps")) adaptiveFps = (val > 0.5f);
            else if (!strcmp(key, "battery_saver")) batterySaver = (val > 0.5f);
            else if (!strcmp(key, "gyro_aiming")) gyroAiming = (val > 0.5f);
            else if (!strcmp(key, "gyro_invert_x")) gyroInvertX = (val > 0.5f);
            else if (!strcmp(key, "gyro_invert_y")) gyroInvertY = (val > 0.5f);
            else if (!strcmp(key, "graphics_backend")) {
                graphicsBackend = val >= 0.5f ? 1 : 0;
            }
            else if (!strcmp(key, "graphics_profile_version")) {
                graphicsProfileVersion = (int)val;
                hasGraphicsProfileVersion = true;
            }
            else if (!strcmp(key, "relative_pointer")) relativePointer = (val > 0.5f);
            else if (!strcmp(key, "screen_orientation")) screenOrientationChoice = std::max(0, std::min(2, (int)val));
        }
    }
    fclose(f);
    port_input_set_camera_sensitivity(rightStickSensitivity);
    port_input_set_camera_inverted(cameraInvertX ? 1 : 0);
    port_input_set_camera_invert_y(cameraInvertY ? 1 : 0);
    bool migratedSettings = false;
    if (!hasGraphicsProfileVersion || graphicsProfileVersion < 1) {
        // The previous default Vulkan path can flicker or crash on some mobile
        // GPU drivers. Move existing installs to the GLES compatibility path,
        // and hide the development HUD for a clean first launch. Game saves and
        // all other touch settings are preserved; the user can still reselect
        // Vulkan and the HUD from Settings.
        graphicsBackend = 0;
        perfHudMode = 0;
        migratedSettings = true;
        LOGI("Migrated display settings to OpenGL ES compatibility mode; game saves preserved");
    }
    if (!hasDisplayFitVersion || displayFitVersion < 2) {
        // The user requested edge-to-edge native aspect as the default. This
        // only changes display preferences; saves and controls remain intact.
        fullscreen = true;
        screenScaleIndex = 2;
        migratedSettings = true;
        LOGI("Migrated display framing to native full-screen Hor+; game saves preserved");
    }
    if (migratedSettings) save(path);
    LOGI("Loaded settings: show=%d, full=%d, screenScale=%d, touch=%d, hud=%d, backend=%d, relPtr=%d, orient=%d, saver=%d, gyro=%d, gyroInvX=%d, gyroInvY=%d, camSens=%.2f, camInvX=%d, camInvY=%d, op=%.2f, scale=%.2f, dpad=%d, fly=%d, skip=%d, infHp=%d, infLives=%d, infBits=%d, spd=%d, lang=%d, res=%d, hap=%d",
          showControls, fullscreen, screenScaleIndex, fullTouchMode, perfHudMode, graphicsBackend, relativePointer ? 1 : 0, screenOrientationChoice, batterySaver ? 1 : 0, gyroAiming ? 1 : 0, gyroInvertX ? 1 : 0, gyroInvertY ? 1 : 0, rightStickSensitivity, cameraInvertX ? 1 : 0, cameraInvertY ? 1 : 0, opacity, scale, showDpad, modFly, modSkipCutscenes, modInfiniteHealth, modInfiniteLives, modInfiniteStarBits, gameSpeed, languageChoice, renderScale, haptics ? 1 : 0);
}

void Settings::save(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "show_controls=%d\n", showControls ? 1 : 0);
    fprintf(f, "achievement_notifications=%d\n", achievementNotifications ? 1 : 0);
    fprintf(f, "fullscreen=%d\n", fullscreen ? 1 : 0);
    fprintf(f, "screen_scale_index=%d\n", std::max(0, std::min(2, screenScaleIndex)));
    fprintf(f, "display_fit_version=2\n");
    fprintf(f, "full_touch_mode=%d\n", fullTouchMode ? 1 : 0);
    fprintf(f, "perf_hud=%d\n", perfHudMode);
    fprintf(f, "graphics_backend=%d\n", graphicsBackend);
    fprintf(f, "graphics_profile_version=1\n");
    fprintf(f, "relative_pointer=%d\n", relativePointer ? 1 : 0);
    fprintf(f, "screen_orientation=%d\n", screenOrientationChoice);
    fprintf(f, "battery_saver=%d\n", batterySaver ? 1 : 0);
    fprintf(f, "gyro_aiming=%d\n", gyroAiming ? 1 : 0);
    fprintf(f, "gyro_invert_x=%d\n", gyroInvertX ? 1 : 0);
    fprintf(f, "gyro_invert_y=%d\n", gyroInvertY ? 1 : 0);
    fprintf(f, "opacity=%.2f\n", opacity);
    fprintf(f, "scale=%.2f\n", scale);
    fprintf(f, "dynamic_stick=%d\n", dynamicStick ? 1 : 0);
    fprintf(f, "stick_offset_x=%.1f\n", stickOffsetX);
    fprintf(f, "stick_offset_y=%.1f\n", stickOffsetY);
    fprintf(f, "button_offset_x=%.1f\n", buttonOffsetX);
    fprintf(f, "button_offset_y=%.1f\n", buttonOffsetY);
    fprintf(f, "btn_a_offset_x=%.1f\n", btnAOffsetX);
    fprintf(f, "btn_a_offset_y=%.1f\n", btnAOffsetY);
    fprintf(f, "btn_b_offset_x=%.1f\n", btnBOffsetX);
    fprintf(f, "btn_b_offset_y=%.1f\n", btnBOffsetY);
    fprintf(f, "btn_z_offset_x=%.1f\n", btnZOffsetX);
    fprintf(f, "btn_z_offset_y=%.1f\n", btnZOffsetY);
    fprintf(f, "btn_spin_offset_x=%.1f\n", btnSpinOffsetX);
    fprintf(f, "btn_spin_offset_y=%.1f\n", btnSpinOffsetY);
    fprintf(f, "pause_offset_x=%.1f\n", pauseOffsetX);
    fprintf(f, "pause_offset_y=%.1f\n", pauseOffsetY);
    fprintf(f, "show_stick=%d\n", showStick ? 1 : 0);
    fprintf(f, "show_button_a=%d\n", showButtonA ? 1 : 0);
    fprintf(f, "show_button_b=%d\n", showButtonB ? 1 : 0);
    fprintf(f, "show_button_z=%d\n", showButtonZ ? 1 : 0);
    fprintf(f, "show_button_spin=%d\n", showButtonSpin ? 1 : 0);
    fprintf(f, "show_pause_button=%d\n", showPauseButton ? 1 : 0);
    fprintf(f, "show_dpad=%d\n", showDpad ? 1 : 0);
    fprintf(f, "dpad_on_right=%d\n", dpadOnRight ? 1 : 0);
    fprintf(f, "dpad_offset_x=%.1f\n", dpadOffsetX);
    fprintf(f, "dpad_offset_y=%.1f\n", dpadOffsetY);
    fprintf(f, "controller_key_a=%d\n", controllerKeyA);
    fprintf(f, "controller_key_b=%d\n", controllerKeyB);
    fprintf(f, "controller_key_c=%d\n", controllerKeyC);
    fprintf(f, "controller_key_z=%d\n", controllerKeyZ);
    fprintf(f, "controller_key_spin=%d\n", controllerKeySpin);
    fprintf(f, "controller_key_pause=%d\n", controllerKeyPause);
    fprintf(f, "controller_deadzone=%.2f\n", controllerDeadzone);
    fprintf(f, "left_stick_sensitivity=%.2f\n", leftStickSensitivity);
    fprintf(f, "right_stick_sensitivity=%.2f\n", rightStickSensitivity);
    fprintf(f, "camera_invert_x=%d\n", cameraInvertX ? 1 : 0);
    fprintf(f, "camera_invert_y=%d\n", cameraInvertY ? 1 : 0);
    fprintf(f, "mod_shield_textures=%d\n", modShieldTextures ? 1 : 0);
    fprintf(f, "mod_shield_cutscenes=%d\n", modShieldCutscenes ? 1 : 0);
    fprintf(f, "mod_shield_controls=%d\n", modShieldControls ? 1 : 0);
    fprintf(f, "mod_classic_gamepad_ui=%d\n", modClassicGamepadUI ? 1 : 0);
    fprintf(f, "mod_riivolution=%d\n", modRiivolution ? 1 : 0);
    fprintf(f, "mod_fly=%d\n", modFly ? 1 : 0);
    fprintf(f, "mod_skip_cutscenes=%d\n", modSkipCutscenes ? 1 : 0);
    fprintf(f, "mod_infinite_health=%d\n", modInfiniteHealth ? 1 : 0);
    fprintf(f, "mod_infinite_lives=%d\n", modInfiniteLives ? 1 : 0);
    fprintf(f, "mod_infinite_starbits=%d\n", modInfiniteStarBits ? 1 : 0);
    fprintf(f, "game_speed=%d\n", gameSpeed);
    fprintf(f, "language_choice=%d\n", languageChoice);
    fprintf(f, "render_scale=%d\n", renderScale);
    fprintf(f, "haptics=%d\n", haptics ? 1 : 0);
    fprintf(f, "target_fps=%d\n", targetFps);
    fprintf(f, "adaptive_fps=%d\n", adaptiveFps ? 1 : 0);
    fclose(f);
    LOGI("Saved settings to %s", path.c_str());
}


UIOverlay::UIOverlay() {
    mPointerX = 0.0f; // Screen center
    mPointerY = 0.0f; // Screen center
}

UIOverlay& UIOverlay::get() {
    static UIOverlay sInstance;
    return sInstance;
}

void UIOverlay::setSavePath(const std::string& path) {
    mSavePath = path;
    mSettings.load(path);
    port_input_set_camera_sensitivity(mSettings.rightStickSensitivity);
    port_input_set_camera_inverted(mSettings.cameraInvertX ? 1 : 0);
    port_input_set_camera_invert_y(mSettings.cameraInvertY ? 1 : 0);
    applyGameSpeed(mSettings.gameSpeed);
    applyLanguage(mSettings.languageChoice);
    applyTargetFps(mSettings.batterySaver ? 30 : mSettings.targetFps);
}

void UIOverlay::setAchievementsPath(const std::string& path) {
    if (mAchievementsPath == path) return;
    mAchievementsPath = path;
    loadAchievementStats();
}

bool UIOverlay::achievementsUseSpanish() const {
    if (mSettings.languageChoice == 2) return true;
    if (mSettings.languageChoice != 0) return false;
#if defined(__ANDROID__)
    char locale[PROP_VALUE_MAX] = {};
    if (__system_property_get("persist.sys.locale", locale) <= 0 &&
        __system_property_get("ro.product.locale", locale) <= 0) {
        __system_property_get("persist.sys.language", locale);
    }
    return (locale[0] == 'e' || locale[0] == 'E') &&
           (locale[1] == 's' || locale[1] == 'S');
#else
    return false;
#endif
}

void UIOverlay::loadAchievementStats() {
    mAchievementFirstGameStarted = false;
    mAchievementTotalPlayedSeconds = 0;
    mAchievementLastPlayedEpochSeconds = 0;
    if (mAchievementsPath.empty()) return;

    FILE* file = fopen(mAchievementsPath.c_str(), "r");
    if (!file) {
        LOGI("achievements: no saved stats yet; starting local record");
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        char* equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = '\0';
        char* end = strpbrk(equals, "\r\n");
        if (end) *end = '\0';
        if (!strcmp(line, "first_game_started")) {
            mAchievementFirstGameStarted = atoi(equals) == 1;
        } else if (!strcmp(line, "total_played_seconds")) {
            mAchievementTotalPlayedSeconds = strtoull(equals, nullptr, 10);
        } else if (!strcmp(line, "last_played_epoch")) {
            mAchievementLastPlayedEpochSeconds = strtoll(equals, nullptr, 10);
        }
    }
    fclose(file);
    LOGI("achievements: loaded local progress first_game=%d total_seconds=%llu",
         mAchievementFirstGameStarted ? 1 : 0,
         static_cast<unsigned long long>(mAchievementTotalPlayedSeconds));
}

void UIOverlay::saveAchievementStats() {
    if (mAchievementsPath.empty()) return;
    const std::string temporaryPath = mAchievementsPath + ".tmp";
    FILE* file = fopen(temporaryPath.c_str(), "w");
    if (!file) {
        LOGE("achievements: cannot write local stats at %s", mAchievementsPath.c_str());
        return;
    }
    bool ok = fprintf(file,
                      "version=1\nfirst_game_started=%d\ntotal_played_seconds=%llu\nlast_played_epoch=%lld\n",
                      mAchievementFirstGameStarted ? 1 : 0,
                      static_cast<unsigned long long>(mAchievementTotalPlayedSeconds),
                      static_cast<long long>(mAchievementLastPlayedEpochSeconds)) > 0;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temporaryPath.c_str(), mAchievementsPath.c_str()) != 0) {
        unlink(temporaryPath.c_str());
        LOGE("achievements: atomic save failed; existing stats kept");
        return;
    }
    mAchievementLastPersistMs = currentTimeMs();
}

void UIOverlay::setGameplayActive(bool active) {
    if (active == mAchievementGameplayActive) return;
    const int64_t nowMs = currentTimeMs();
    if (active) {
        mAchievementGameplayActive = true;
        mAchievementResumeAfterPause = false;
        mAchievementSessionStartMs = nowMs;
        mAchievementLastPersistMs = nowMs;
        mAchievementLastPlayedEpochSeconds = static_cast<int64_t>(time(nullptr));
        if (!mAchievementFirstGameStarted) {
            mAchievementFirstGameStarted = true;
            LOGI("achievements: unlocked First Adventure (first gameplay scene started)");
            if (mSettings.achievementNotifications) {
                mAchievementToastMessage = achievementsUseSpanish()
                    ? "Logro desbloqueado: Primera aventura"
                    : "Achievement unlocked: First Adventure";
                mAchievementToastUntilMs = nowMs + 3000;
                mDirty = true;
            }
        }
        LOGI("achievements: gameplay session started");
        saveAchievementStats();
    } else {
        const uint64_t elapsedSeconds =
            nowMs > mAchievementSessionStartMs
                ? static_cast<uint64_t>(nowMs - mAchievementSessionStartMs) / 1000
                : 0;
        mAchievementTotalPlayedSeconds += elapsedSeconds;
        mAchievementGameplayActive = false;
        mAchievementResumeAfterPause = false;
        mAchievementSessionStartMs = 0;
        LOGI("achievements: gameplay session ended; total_seconds=%llu",
             static_cast<unsigned long long>(mAchievementTotalPlayedSeconds));
        saveAchievementStats();
    }
    mDirty = true;
}

void UIOverlay::pauseAchievementSession() {
    if (!mAchievementGameplayActive) return;
    const int64_t nowMs = currentTimeMs();
    const uint64_t elapsedSeconds =
        nowMs > mAchievementSessionStartMs
            ? static_cast<uint64_t>(nowMs - mAchievementSessionStartMs) / 1000
            : 0;
    mAchievementTotalPlayedSeconds += elapsedSeconds;
    mAchievementGameplayActive = false;
    mAchievementResumeAfterPause = true;
    mAchievementSessionStartMs = 0;
    LOGI("achievements: session paused; total_seconds=%llu",
         static_cast<unsigned long long>(mAchievementTotalPlayedSeconds));
    saveAchievementStats();
}

void UIOverlay::resumeAchievementSession() {
    if (!mAchievementResumeAfterPause || !mAchievementFirstGameStarted) return;
    mAchievementGameplayActive = true;
    mAchievementResumeAfterPause = false;
    mAchievementSessionStartMs = currentTimeMs();
    mAchievementLastPersistMs = mAchievementSessionStartMs;
    mAchievementLastPlayedEpochSeconds = static_cast<int64_t>(time(nullptr));
    LOGI("achievements: gameplay session resumed");
    saveAchievementStats();
}

void UIOverlay::tickAchievementSession() {
    if (!mAchievementGameplayActive) return;
    const int64_t nowMs = currentTimeMs();
    if (nowMs - mAchievementLastPersistMs < 30000) return;
    if (nowMs > mAchievementSessionStartMs) {
        const uint64_t elapsedSeconds =
            static_cast<uint64_t>(nowMs - mAchievementSessionStartMs) / 1000;
        mAchievementTotalPlayedSeconds += elapsedSeconds;
        mAchievementSessionStartMs += static_cast<int64_t>(elapsedSeconds) * 1000;
    }
    mAchievementLastPersistMs = nowMs;
    saveAchievementStats();
    mDirty = true;
}

void UIOverlay::reportFramePresented() {
    mFpsFrameCounter++;
}

bool UIOverlay::needsRedraw() {
    float target = mSettingsOpen ? 1.0f : 0.0f;
    if (fabsf(mDrawerAnim - target) > 0.001f) {
        return true;
    }
    bool d = mDirty;
    mDirty = false;
    return d;
}

void UIOverlay::closeSettings() {
    mSettingsOpen = false;
    mOverwriteConfirmSlot = 0;
    mOverwriteConfirmUntilMs = 0;
    mDirty = true;
    if (!mSavePath.empty()) {
        mSettings.save(mSavePath);
    }
}

void UIOverlay::triggerSaveAnywhere() {
    triggerSaveSlot(4, true);
}

void UIOverlay::triggerSaveSlot(int slot, bool allowQuickOverwrite) {
    if (slot < 1 || slot > 5) return;
    const int64_t now = currentTimeMs();
    if (!allowQuickOverwrite && port_nand_save_slot_exists(slot)) {
        if (mOverwriteConfirmSlot != slot || now > mOverwriteConfirmUntilMs) {
            mOverwriteConfirmSlot = slot;
            mOverwriteConfirmUntilMs = now + 4000;
            char confirm[104];
            snprintf(confirm, sizeof(confirm),
                     "Ranura %d tiene datos; toca Guardar otra vez para reemplazarla", slot);
            showToast(confirm);
            mDirty = true;
            return;
        }
    }
    mOverwriteConfirmSlot = 0;
    mOverwriteConfirmUntilMs = 0;
    port_trigger_quick_save(slot);
    if (mSettings.haptics) port_android_vibrate(50);
    char msg[80];
    snprintf(msg, sizeof(msg), "Guardando ranura %d; espera confirmacion", slot);
    showToast(msg);
    LOGI("Queued safe game-data save for slot %d", slot);
}

void UIOverlay::triggerLoadSlot(int slot) {
    if (slot < 1 || slot > 5) return;
    mOverwriteConfirmSlot = 0;
    mOverwriteConfirmUntilMs = 0;
    if (!port_nand_save_slot_exists(slot)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Ranura %d vacia o invalida", slot);
        showToast(msg);
        LOGE("Refused load from empty/invalid save slot %d", slot);
        return;
    }
    port_trigger_quick_load(slot);
    if (mSettings.haptics) port_android_vibrate(75);
    char msg[80];
    snprintf(msg, sizeof(msg), "Preparando carga segura de ranura %d", slot);
    showToast(msg);
    LOGI("Queued safe game-data load for slot %d", slot);
}

extern "C" void port_mobile_save_slot_result(int slot, int success) {
    char msg[96];
    snprintf(msg, sizeof(msg), success ? "Ranura %d guardada" : "Error al guardar ranura %d; revisa el registro", slot);
    get().showToast(msg);
}

extern "C" void port_mobile_load_slot_result(int slot, int success) {
    char msg[96];
    const char* text = success > 0
        ? "Ranura %d cargada; partida iniciada"
        : (success == 0
               ? "Fallo al cargar ranura %d; guardado anterior restaurado"
               : "Fallo al cargar ranura %d; rollback no confirmado: no guardes");
    snprintf(msg, sizeof(msg), text, slot);
    get().showToast(msg);
}

extern "C" void port_mobile_achievement_set_gameplay_active(int active) {
    UIOverlay::get().setGameplayActive(active != 0);
}

extern "C" void port_mobile_achievement_tick(void) {
    UIOverlay::get().tickAchievementSession();
}

void UIOverlay::showToast(const std::string& msg) {
    mToastMessage = msg;
    mSaveToastTimer = 2.8f;
    mDirty = true;
}

void UIOverlay::setGamepadConnected(bool connected) {
    if (mGamepadConnected == connected) return;
    mGamepadConnected = connected;
    if (connected) {
        // Do not leave a virtual button or stick held if a controller is
        // connected while the player is touching the screen.
        for (auto& touch : mTouches) touch = {};
        mStickPointerId = -1;
        mTouchPointerId = -1;
        mTouchStickX = 0.0f;
        mTouchStickY = 0.0f;
        mTapPendingFrames = 0;
        mFingerDownOnA = mFingerDownOnB = mFingerDownOnZ = false;
        mFingerDownOnSpin = mFingerDownOnPlus = false;
    }
    mDirty = true;
}

static bool containsLogToken(const char* text, size_t length, const char* token) {
    const size_t tokenLength = strlen(token);
    if (!text || tokenLength == 0 || tokenLength > length) return false;
    for (size_t i = 0; i + tokenLength <= length; ++i) {
        size_t j = 0;
        while (j < tokenLength &&
               std::toupper((unsigned char)text[i + j]) == token[j]) {
            ++j;
        }
        if (j == tokenLength) return true;
    }
    return false;
}

static bool isDiagnosticWarning(const char* text, size_t length) {
    return containsLogToken(text, length, "WARN") &&
           !containsLogToken(text, length, " INFO ");
}

static bool isDiagnosticError(const char* text, size_t length) {
    // Classify the log record, not words inside an informational message.
    // Give explicit WARN records priority over text such as "may have crashed".
    if (containsLogToken(text, length, " INFO ") ||
        isDiagnosticWarning(text, length)) {
        return false;
    }

    return containsLogToken(text, length, "ERROR") ||
           containsLogToken(text, length, "FATAL") ||
           containsLogToken(text, length, "FAILED") ||
           containsLogToken(text, length, "FAILURE") ||
           containsLogToken(text, length, "CRASH") ||
           containsLogToken(text, length, "EXCEPTION") ||
           containsLogToken(text, length, "SIGSEGV") ||
           containsLogToken(text, length, "SIGABRT") ||
           containsLogToken(text, length, "DEVICE LOST") ||
           containsLogToken(text, length, "ANR") ||
           containsLogToken(text, length, "UNCAUGHT");
}

void UIOverlay::refreshLogCache(bool force) {
    const int64_t now = currentTimeMs();
    if (!force && mLogLastRefreshMs > 0 && now - mLogLastRefreshMs < 750) return;

    // Read directly into the owned string; storing line offsets below avoids
    // a second 2.5 MB copy of the ring in filtered strings.
    mLogText.resize(PORT_LOG_BUFFER_CAPACITY);
    const int bytes = port_get_recent_logs(mLogText.data(), (int)mLogText.size());
    if (bytes > 0) {
        const size_t safeBytes = std::min<size_t>((size_t)bytes, mLogText.size() - 1);
        mLogText.resize(safeBytes);
    } else {
        mLogText.clear();
    }

    mLogLineCount = 0;
    mLogErrorCount = 0;
    mLogWarningCount = 0;
    mLogFilteredLines.clear();
    size_t start = 0;
    while (start < mLogText.size()) {
        const size_t end = mLogText.find('\n', start);
        const size_t length = (end == std::string::npos ? mLogText.size() : end) - start;
        if (length > 0) {
            mLogLineCount++;
            const char* line = mLogText.data() + start;
            const auto has = [&](const char* token) {
                return containsLogToken(line, length, token);
            };
            const bool isError = isDiagnosticError(line, length);
            const bool isWarning = isDiagnosticWarning(line, length);
            if (isError) {
                mLogErrorCount++;
            } else if (isWarning) {
                mLogWarningCount++;
            }

            bool include = mLogFilter == 0;
            if (mLogFilter == 1) {
                include = has("SAVE") || has("NAND") ||
                          has("GAMEDATA") || has("SLOT");
            } else if (mLogFilter == 2) {
                include = has("LOAD") || has("BOOT") || has("MOUNT") ||
                          has("ARCHIVE") || has("RESOURCE") || has("ASSET") ||
                          has("READ ") || has("OPENING") || has("DECOMPRESS") ||
                          has("PARSE") || has("FILESELECT") || has("DATA ROOT");
            } else if (mLogFilter == 3) {
                include = has("GFX") || has("GL:") || has("VK:") ||
                          has("GPU") || has("TEXTURE") || has("SHADER") ||
                          has("FRAMEBUFFER") || has("RENDERER") || has("FRAME ");
            } else if (mLogFilter == 4) {
                include = has("AUDIO") || has("DSP") || has("AAUDIO") ||
                          has("SOUND") || has("WAVE") || has("MUSIC");
            } else if (mLogFilter == 5) {
                include = isError || isWarning;
            }
            if (include) {
                mLogFilteredLines.push_back(
                    {static_cast<uint32_t>(start), static_cast<uint32_t>(length)});
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    mLogLastRefreshMs = now;
}

bool UIOverlay::init() {
    if (mInitialized) return true;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;

    mProg = glCreateProgram();
    glAttachShader(mProg, vs);
    glAttachShader(mProg, fs);
    glLinkProgram(mProg);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(mProg, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(mProg, sizeof(log), nullptr, log);
        LOGE("UI program link error: %s", log);
        return false;
    }

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);

    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);

    // layout 0: pos (vec2)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, x));

    // layout 1: uv (vec2)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, u));

    // layout 2: color (vec4)
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, r));

    // layout 3: params (vec4)
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(UIVertex), (void*)offsetof(UIVertex, mode));

    glBindVertexArray(0);

    // Decode font atlas into GL_R8 texture
    std::vector<uint8_t> coverage;
    getFontAtlas(mAtlasW, mAtlasH, coverage);

    glGenTextures(1, &mFontTex);
    glBindTexture(GL_TEXTURE_2D, mFontTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mAtlasW, mAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, coverage.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const char* rend = (const char*)glGetString(GL_RENDERER);
    if (rend && rend[0]) {
        mGpuRendererName = rend;
    }

    mInitialized = true;
    LOGI("Mobile UI initialized successfully (Atlas %dx%d, GPU: %s)", mAtlasW, mAtlasH, mGpuRendererName.c_str());
    return true;
}

void UIOverlay::getFontAtlas(int& width, int& height, std::vector<uint8_t>& coverage) {
    const ui_font::Face& face = ui_font::kFaces[0]; // size 40 Inter Bold
    width = face.width;
    height = face.height;

    std::vector<uint8_t> packed;
    uint32_t bits = 0;
    int nbits = 0;
    for (const char* p = face.atlas; *p; p++) {
        int v = base64Val(*p);
        if (v < 0) continue;
        bits = (bits << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            packed.push_back((uint8_t)(bits >> nbits));
        }
    }

    coverage.assign((size_t)face.width * face.height, 0);
    for (size_t i = 0; i < coverage.size(); i++) {
        uint8_t b = (i / 2 < packed.size()) ? packed[i / 2] : 0;
        coverage[i] = (uint8_t)(((i & 1) ? (b & 15) : (b >> 4)) * 17);
    }
}

uint32_t UIOverlay::getTouchButtons() const {
    if (mGamepadConnected) return 0;
    uint32_t b = 0;
    if ((mSettings.showControls && mSettings.showButtonA && mFingerDownOnA) || mTapPendingFrames > 0) b |= 0x0800;
    if ((mSettings.showControls && mSettings.showButtonB && mFingerDownOnB) ||
        (mTapPendingFrames > 0 && port_is_title_screen())) b |= 0x0400;
    if (mSettings.showControls && mSettings.showButtonZ && mFingerDownOnZ) b |= 0x2000;
    if (mSettings.showControls && mSettings.showPauseButton && mFingerDownOnPlus) b |= 0x0010;
    if (mSettings.showControls && mSettings.showDpad) {
        if (mBtnDpadLeft) b |= 0x0001;
        if (mBtnDpadRight) b |= 0x0002;
        if (mBtnDpadDown) b |= 0x0004;
        if (mBtnDpadUp) b |= 0x0008;
    }
    return b;
}

void UIOverlay::update(PortPadState& pad, int& spinFrames) {
    // Diagnostic hook: open the layout editor straight away so it can be driven
    // from `adb shell setprop debug.petari.editlayout 1` without walking the menu.
    if (!mDebugEditChecked) {
        mDebugEditChecked = true;
        char value[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.petari.editlayout", value) > 0 && value[0] == '1') {
            mEditLayoutMode = true;
            mSettingsOpen = false;
            mDirty = true;
            LOGI("layout edit: opened by debug.petari.editlayout");
        }
    }

    // 1. Decay virtual tap pulse for full-touch controls (Title screen only)
    if (mTapDelayFrames > 0) {
        mTapDelayFrames--;
    } else if (mTapPendingFrames > 0) {
        mTapPendingFrames--;
    }

    // Direct button visual press states (without clearing physical controller bits)
    mBtnAPressed = mFingerDownOnA || (mTapPendingFrames > 0);
    mBtnBPressed = mFingerDownOnB || (mTapPendingFrames > 0 && port_is_title_screen());
    mBtnZPressed = mFingerDownOnZ;

    if (mFingerDownOnSpin) {
        spinFrames = 6;
        mBtnSpinPressed = true;
    } else if (spinFrames <= 0) {
        mBtnSpinPressed = false;
    }

    mBtnPlusPressed = mFingerDownOnPlus;

    // Always maintain valid pointer position
    pad.pointerValid = 1;
    pad.pointerX = mPointerX;
    pad.pointerY = mPointerY;
    pad.pointerDist = 2.0f;

    // Toast Timer Tick
    if (mSaveToastTimer > 0.0f) {
        mSaveToastTimer -= 0.016f;
        if (mSaveToastTimer < 0.0f) mSaveToastTimer = 0.0f;
        mDirty = true;
    }

    // 2. Hardware and Performance Monitoring Metrics (FPS, Frame Time, CPU, RAM)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t nowNs = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    if (mFpsLastTimeNs == 0) mFpsLastTimeNs = nowNs;
    int64_t diffNs = nowNs - mFpsLastTimeNs;
    if (diffNs >= 500000000ll) { // Sample every 500ms
        float sec = (float)diffNs / 1e9f;
        mCurrentFps = (float)mFpsFrameCounter / sec;
        int activeTargetFps = mSettings.batterySaver ? 30 : mSettings.targetFps;
        if (port_get_game_speed() <= 1.05f && mCurrentFps > (float)activeTargetFps) {
            mCurrentFps = (float)activeTargetFps;
        }
        mAvgFrameTimeMs = 1000.0f / (mCurrentFps > 1.0f ? mCurrentFps : 1.0f);
        mFpsFrameCounter = 0;
        mFpsLastTimeNs = nowNs;

        // Sample CPU usage:
        // 1. Try global /proc/stat (available on older Android or rooted/permissive ROMs)
        bool gotCpu = false;
        static unsigned long long prevUser = 0, prevNice = 0, prevSystem = 0, prevIdle = 0;
        FILE* fpStat = fopen("/proc/stat", "r");
        if (fpStat) {
            unsigned long long u = 0, n = 0, s = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
            if (fscanf(fpStat, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &iowait, &irq, &softirq) >= 4) {
                unsigned long long total = (u + n + s + idle + iowait + irq + softirq);
                unsigned long long prevTotal = (prevUser + prevNice + prevSystem + prevIdle);
                if (prevTotal > 0 && total > prevTotal) {
                    unsigned long long totalDiff = total - prevTotal;
                    unsigned long long idleDiff = (idle + iowait) - prevIdle;
                    if (totalDiff > 0 && idleDiff <= totalDiff) {
                        mCpuUsagePercent = (float)(totalDiff - idleDiff) * 100.0f / (float)totalDiff;
                        gotCpu = true;
                    }
                }
                prevUser = u + n;
                prevSystem = s + irq + softirq;
                prevIdle = idle + iowait;
            }
            fclose(fpStat);
        }

        // 2. SELinux on Android 8+ blocks app access to /proc/stat. Fall back to process CPU via /proc/self/stat
        if (!gotCpu) {
            static unsigned long long prevProcTicks = 0;
            FILE* fpSelf = fopen("/proc/self/stat", "r");
            if (fpSelf) {
                char statBuf[512];
                if (fgets(statBuf, sizeof(statBuf), fpSelf)) {
                    char* paren = strrchr(statBuf, ')');
                    if (paren) {
                        unsigned long long utime = 0, stime = 0;
                        // Fields after ')': state, ppid, pgrp, session, tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime
                        if (sscanf(paren + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) == 2) {
                            unsigned long long totalTicks = utime + stime;
                            if (prevProcTicks > 0 && totalTicks >= prevProcTicks && sec > 0.001f) {
                                long clkTck = sysconf(_SC_CLK_TCK);
                                if (clkTck <= 0) clkTck = 100;
                                long numCores = sysconf(_SC_NPROCESSORS_ONLN);
                                if (numCores <= 0) numCores = 1;
                                float cpuSec = (float)(totalTicks - prevProcTicks) / (float)clkTck;
                                float usage = (cpuSec / sec) * 100.0f / (float)numCores;
                                if (usage < 0.0f) usage = 0.0f;
                                if (usage > 100.0f) usage = 100.0f;
                                mCpuUsagePercent = usage;
                            }
                            prevProcTicks = totalTicks;
                        }
                    }
                }
                fclose(fpSelf);
            }
        }

        // Sample RAM usage from /proc/self/statm
        FILE* fpStatm = fopen("/proc/self/statm", "r");
        if (fpStatm) {
            unsigned long sizePages = 0, residentPages = 0;
            if (fscanf(fpStatm, "%lu %lu", &sizePages, &residentPages) == 2) {
                // 4KB page size
                mRamUsageMB = (float)(residentPages * 4096) / (1024.0f * 1024.0f);
            }
            fclose(fpStatm);
        }

        // Sample GPU usage
        bool gotGpu = false;
        static const char* const gpuSysfsPaths[] = {
            "/sys/kernel/ged/hal/gpu_utilization",
            "/sys/class/misc/mali0/device/utilization",
            "/sys/devices/platform/13000000.mali/utilization",
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"
        };
        for (const char* path : gpuSysfsPaths) {
            FILE* fpGpu = fopen(path, "r");
            if (fpGpu) {
                int val = -1;
                if (fscanf(fpGpu, "%d", &val) == 1 && val >= 0) {
                    mGpuUsagePercent = (float)(val > 100 ? 100 : val);
                    gotGpu = true;
                }
                fclose(fpGpu);
                if (gotGpu) break;
            }
        }
        if (!gotGpu) {
            // Render duty cycle fallback: measured active GPU render time vs window duration
            if (diffNs > 0 && mGpuRenderFrameCount > 0) {
                float gpuDuty = ((float)mGpuRenderTimeAccumNs / (float)diffNs) * 100.0f;
                if (gpuDuty < 0.0f) gpuDuty = 0.0f;
                if (gpuDuty > 100.0f) gpuDuty = 100.0f;
                mGpuUsagePercent = gpuDuty;
            } else if (mCurrentFps > 0.0f) {
                // Approximate fallback based on frame pacing
                mGpuUsagePercent = std::min(100.0f, (mAvgFrameTimeMs / 16.67f) * 40.0f);
            }
        }
        mGpuRenderTimeAccumNs = 0;
        mGpuRenderFrameCount = 0;

        mDirty = true;
    }
}

UIOverlay::ButtonLayout UIOverlay::buttonLayout(int screenW, int screenH) const {
    float baseDpi = (float)screenH / 650.0f;
    float scale = baseDpi * mSettings.scale;
    float w = (float)screenW;
    float h = (float)screenH;

    // Shared anchor of the action pad.  Dragging the pad in the layout editor
    // moves it; the per-button offsets below move a single button.
    float baseX = w - 150.0f * scale + mSettings.buttonOffsetX;
    float baseY = h - 150.0f * scale + mSettings.buttonOffsetY;

    ButtonLayout l{};

    // A (Jump) - primary action, the pad anchor
    l.ax = baseX + mSettings.btnAOffsetX;
    l.ay = baseY + mSettings.btnAOffsetY;
    l.ar = 54.0f * scale;

    // B (Shoot Star Bit) - left of A, elevated comfortably
    l.bx = baseX - 118.0f * scale + mSettings.btnBOffsetX;
    l.by = baseY - 14.0f * scale + mSettings.btnBOffsetY;
    l.br = 45.0f * scale;

    // Z (Crouch) - upper left of A, far enough to press Z+A with two fingers
    l.zx = baseX - 98.0f * scale + mSettings.btnZOffsetX;
    l.zy = baseY - 132.0f * scale + mSettings.btnZOffsetY;
    l.zr = 44.0f * scale;

    // SPIN - above A, shifted slightly right for a natural thumb roll
    l.spinX = baseX + 34.0f * scale + mSettings.btnSpinOffsetX;
    l.spinY = baseY - 128.0f * scale + mSettings.btnSpinOffsetY;
    l.spinR = 48.0f * scale;

    l.pauseX = w - 65.0f * scale + mSettings.pauseOffsetX;
    l.pauseY = 65.0f * scale + mSettings.pauseOffsetY;
    l.pauseR = 34.0f * scale;

    return l;
}

bool UIOverlay::handleInput(const AInputEvent* event, PortPadState& pad, int& spinFrames,
                            int screenW, int screenH, int vpX, int vpY, int vpW, int vpH) {
    int32_t eventType = AInputEvent_getType(event);
    if (eventType != AINPUT_EVENT_TYPE_MOTION) {
        return false;
    }

    int32_t actionWithIndex = AMotionEvent_getAction(event);
    int32_t action = actionWithIndex & AMOTION_EVENT_ACTION_MASK;
    int32_t pointerIndex = (actionWithIndex & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t pointerCount = AMotionEvent_getPointerCount(event);

    // Responsive scaling based on screen height
    float baseDpi = (float)screenH / 650.0f;
    float uiScale = baseDpi;
    float scale = baseDpi * mSettings.scale;
    float w = (float)screenW;
    float h = (float)screenH;
    // Material 3 Settings Tab Dock (Top-Left Edge)
    float tabW = 56.0f * uiScale;
    float tabH = 48.0f * uiScale;
    float tabY = 22.0f * uiScale;

    // Side Sheet Drawer Dimensions; shared with rendering so touch geometry
    // stays aligned with the wider responsive panel.
    float drawerW = settingsDrawerWidth(w, h, uiScale);
    float drawerX = (mDrawerAnim - 1.0f) * drawerW;

    if (mAchievementClearConfirm) {
        if (action == AMOTION_EVENT_ACTION_CANCEL) {
            mAchievementClearConfirm = false;
            mDirty = true;
            return true;
        }
        if (action == AMOTION_EVENT_ACTION_UP) {
            const float px = AMotionEvent_getX(event, pointerIndex);
            const float py = AMotionEvent_getY(event, pointerIndex);
            const float dialogW = std::min(470.0f * uiScale, w - 32.0f * uiScale);
            const float dialogH = 190.0f * uiScale;
            const float dialogX0 = (w - dialogW) * 0.5f;
            const float dialogX1 = dialogX0 + dialogW;
            const float dialogY0 = (h - dialogH) * 0.5f;
            const float dialogY1 = dialogY0 + dialogH;
            const float buttonY0 = dialogY1 - 60.0f * uiScale;
            const float buttonY1 = dialogY1 - 14.0f * uiScale;
            const float splitX = (dialogX0 + dialogX1) * 0.5f;
            const bool onButtons = py >= buttonY0 && py <= buttonY1 &&
                                   px >= dialogX0 + 16.0f * uiScale &&
                                   px <= dialogX1 - 16.0f * uiScale;
            if (onButtons && px > splitX) {
                mAchievementFirstGameStarted = false;
                saveAchievementStats();
                LOGI("achievements: user cleared earned achievement flags; playtime retained");
                showToast(achievementsUseSpanish()
                              ? "Logros borrados; tiempo jugado conservado"
                              : "Achievements cleared; playtime kept");
            }
            mAchievementClearConfirm = false;
            mDirty = true;
        }
        return true;
    }

    // Check Log Viewer input (modal dialog)
    if (mLogViewerOpen) {
        const float px = AMotionEvent_getX(event, pointerIndex);
        const float py = AMotionEvent_getY(event, pointerIndex);
        const float modalPad = 24.0f * uiScale;
        const float headerH = 46.0f * uiScale;
        const float closeX = w - modalPad - 24.0f * uiScale;
        const float closeY = modalPad + 22.0f * uiScale;
        const float dx = px - closeX, dy = py - closeY;
        const float filterX0 = modalPad + 12.0f * uiScale;
        const float filterX1 = w - modalPad - 12.0f * uiScale;
        const float filterY0 = modalPad + headerH + 26.0f * uiScale;
        const float filterH = 24.0f * uiScale;
        const float copyX0 = modalPad + 20.0f * uiScale;
        const float copyX1 = w - modalPad - 20.0f * uiScale;
        const float copyY0 = h - modalPad - 58.0f * uiScale;
        const float copyY1 = h - modalPad - 12.0f * uiScale;

        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            if (py >= filterY0 && py <= filterY0 + filterH &&
                px >= filterX0 && px <= filterX1) {
                const float filterW = (filterX1 - filterX0) / 6.0f;
                mLogFilter = std::max(0, std::min(5, (int)((px - filterX0) / filterW)));
                mLogScrollY = 0.0f;
                refreshLogCache(true);
                mDirty = true;
            } else if (px >= copyX0 && px <= copyX1 && py >= copyY0 && py <= copyY1) {
                refreshLogCache(true);
                const int copied = port_android_copy_to_clipboard(
                    mLogText.data(), (int)mLogText.size());
                if (copied) {
                    LOGI("Copied diagnostics to clipboard (%d lines, %d errors)",
                         mLogLineCount, mLogErrorCount);
                    showToast("Registro completo copiado");
                } else {
                    LOGE("Could not copy diagnostics to Android clipboard");
                    showToast("No se pudo copiar el registro");
                }
                mDirty = true;
            } else if (dx * dx + dy * dy <= 30.0f * uiScale * 30.0f * uiScale ||
                       px < modalPad || px > w - modalPad || py < modalPad || py > h - modalPad) {
                mLogViewerOpen = false;
                mLogTouchScrolling = false;
                mDirty = true;
            } else if (py > filterY0 + filterH && py < copyY0) {
                mLogTouchScrolling = true;
                mLogTouchLastY = py;
            }
        } else if (action == AMOTION_EVENT_ACTION_MOVE && mLogTouchScrolling) {
            const float lineStep = 15.0f * uiScale;
            const float footerY0 = h - modalPad - 58.0f * uiScale;
            const float contentY0 = filterY0 + filterH + 8.0f * uiScale;
            const int visibleLines = std::max(1, (int)((footerY0 - contentY0) / lineStep));
            const int maxScroll = std::max(0, (int)mLogFilteredLines.size() - visibleLines);
            mLogScrollY = std::max(0.0f, std::min((float)maxScroll,
                mLogScrollY + (mLogTouchLastY - py) / lineStep));
            mLogTouchLastY = py;
            mDirty = true;
        } else if (action == AMOTION_EVENT_ACTION_UP ||
                   action == AMOTION_EVENT_ACTION_POINTER_UP ||
                   action == AMOTION_EVENT_ACTION_CANCEL) {
            mLogTouchScrolling = false;
            mDirty = true;
        }
        return true;
    }

    // Check Settings Tab tap (when drawer is closed or nearly closed)
    if (!mSettingsOpen && mDrawerAnim < 0.15f) {
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);
            if (px >= 0.0f && px <= tabW + 16.0f * uiScale && py >= tabY - 8.0f * uiScale && py <= tabY + tabH + 8.0f * uiScale) {
                mSettingsOpen = true;
                mBtnCfgPressed = true;
                mDirty = true;
                return true;
            }
            float qsX = tabW + 16.0f * uiScale;
            float qsW = 54.0f * uiScale;
            if (px >= qsX - 4.0f * uiScale && px <= qsX + qsW + 6.0f * uiScale && py >= tabY - 6.0f * uiScale && py <= tabY + tabH + 6.0f * uiScale) {
                triggerSaveAnywhere();
                return true;
            }
        }
    } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
        mBtnCfgPressed = false;
    }

    // Handle Edit Layout Mode
    if (mEditLayoutMode) {
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);

            // Check Top buttons: Save or Reset
            float saveBtnX = w * 0.5f - 110.0f, saveBtnY = 50.0f;
            if (px >= saveBtnX && px <= saveBtnX + 100.0f && py >= saveBtnY && py <= saveBtnY + 50.0f) {
                mEditLayoutMode = false;
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }
            float rstBtnX = w * 0.5f + 10.0f, rstBtnY = 50.0f;
            if (px >= rstBtnX && px <= rstBtnX + 100.0f && py >= rstBtnY && py <= rstBtnY + 50.0f) {
                clearLayoutOffsets();
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }

            // A touch that starts on an action button moves only that button.
            ButtonLayout bl = buttonLayout(screenW, screenH);
            struct BtnHit { int id; float cx, cy, r; bool visible; } hits[5] = {
                {0, bl.ax, bl.ay, bl.ar, mSettings.showButtonA},
                {1, bl.bx, bl.by, bl.br, mSettings.showButtonB},
                {2, bl.zx, bl.zy, bl.zr, mSettings.showButtonZ},
                {3, bl.spinX, bl.spinY, bl.spinR, mSettings.showButtonSpin},
                {4, bl.pauseX, bl.pauseY, bl.pauseR, mSettings.showPauseButton},
            };
            for (const BtnHit& hit : hits) {
                if (!hit.visible) continue;
                float hitR = hit.r * 1.15f;
                float d2 = (px - hit.cx) * (px - hit.cx) + (py - hit.cy) * (py - hit.cy);
                if (d2 > hitR * hitR) continue;

                mDraggingButton = hit.id;
                const char* buttonName = hit.id == 0 ? "A" : hit.id == 1 ? "B" :
                                         hit.id == 2 ? "Z" : hit.id == 3 ? "SPIN" : "PAUSE";
                LOGI("layout edit: dragging button %s from %.0f,%.0f",
                     buttonName, px, py);
                mDragStartX = px;
                mDragStartY = py;
                switch (hit.id) {
                case 0: mInitialOwnOfsX = mSettings.btnAOffsetX; mInitialOwnOfsY = mSettings.btnAOffsetY; break;
                case 1: mInitialOwnOfsX = mSettings.btnBOffsetX; mInitialOwnOfsY = mSettings.btnBOffsetY; break;
                case 2: mInitialOwnOfsX = mSettings.btnZOffsetX; mInitialOwnOfsY = mSettings.btnZOffsetY; break;
                case 3: mInitialOwnOfsX = mSettings.btnSpinOffsetX; mInitialOwnOfsY = mSettings.btnSpinOffsetY; break;
                default: mInitialOwnOfsX = mSettings.pauseOffsetX; mInitialOwnOfsY = mSettings.pauseOffsetY; break;
                }
                mDirty = true;
                return true;
            }

            // Start dragging stick cluster, button cluster, or D-Pad cluster
            float baseDpadX = mSettings.dpadOnRight ? (w - 380.0f * scale) : (140.0f * scale);
            float baseDpadY = mSettings.dpadOnRight ? (h - 220.0f * scale) : (h - 390.0f * scale);
            float curDpadX = baseDpadX + mSettings.dpadOffsetX;
            float curDpadY = baseDpadY + mSettings.dpadOffsetY;

            float curStickX = 175.0f * scale + mSettings.stickOffsetX;
            float curStickY = h - 175.0f * scale + mSettings.stickOffsetY;

            float curBtnX = w - 160.0f * scale + mSettings.buttonOffsetX - 60.0f * scale;
            float curBtnY = h - 160.0f * scale + mSettings.buttonOffsetY - 60.0f * scale;

            float dDpad2 = (px - curDpadX) * (px - curDpadX) + (py - curDpadY) * (py - curDpadY);
            float dStick2 = (px - curStickX) * (px - curStickX) + (py - curStickY) * (py - curStickY);

            if (mSettings.showDpad && dDpad2 <= (95.0f * scale) * (95.0f * scale)) {
                mDraggingCluster = 3; // D-Pad
                mDragStartX = px;
                mDragStartY = py;
                mInitialDpadOfsX = mSettings.dpadOffsetX;
                mInitialDpadOfsY = mSettings.dpadOffsetY;
            } else if (mSettings.showStick &&
                       (dStick2 <= (140.0f * scale) * (140.0f * scale) || px < w * 0.40f)) {
                mDraggingCluster = 1; // stick
                mDragStartX = px;
                mDragStartY = py;
                mInitialStickOfsX = mSettings.stickOffsetX;
                mInitialStickOfsY = mSettings.stickOffsetY;
            } else {
                mDraggingCluster = 2; // button cluster
                mDragStartX = px;
                mDragStartY = py;
                mInitialBtnOfsX = mSettings.buttonOffsetX;
                mInitialBtnOfsY = mSettings.buttonOffsetY;
            }
            mDirty = true;
            return true;
        } else if (action == AMOTION_EVENT_ACTION_MOVE) {
            float px = AMotionEvent_getX(event, 0);
            float py = AMotionEvent_getY(event, 0);
            if (mDraggingButton >= 0) {
                float dx = px - mDragStartX;
                float dy = py - mDragStartY;
                switch (mDraggingButton) {
                case 0:
                    mSettings.btnAOffsetX = mInitialOwnOfsX + dx;
                    mSettings.btnAOffsetY = mInitialOwnOfsY + dy;
                    break;
                case 1:
                    mSettings.btnBOffsetX = mInitialOwnOfsX + dx;
                    mSettings.btnBOffsetY = mInitialOwnOfsY + dy;
                    break;
                case 2:
                    mSettings.btnZOffsetX = mInitialOwnOfsX + dx;
                    mSettings.btnZOffsetY = mInitialOwnOfsY + dy;
                    break;
                case 3:
                    mSettings.btnSpinOffsetX = mInitialOwnOfsX + dx;
                    mSettings.btnSpinOffsetY = mInitialOwnOfsY + dy;
                    break;
                default:
                    mSettings.pauseOffsetX = mInitialOwnOfsX + dx;
                    mSettings.pauseOffsetY = mInitialOwnOfsY + dy;
                    break;
                }
            } else if (mDraggingCluster == 1) {
                mSettings.stickOffsetX = mInitialStickOfsX + (px - mDragStartX);
                mSettings.stickOffsetY = mInitialStickOfsY + (py - mDragStartY);
            } else if (mDraggingCluster == 2) {
                mSettings.buttonOffsetX = mInitialBtnOfsX + (px - mDragStartX);
                mSettings.buttonOffsetY = mInitialBtnOfsY + (py - mDragStartY);
            } else if (mDraggingCluster == 3) {
                mSettings.dpadOffsetX = mInitialDpadOfsX + (px - mDragStartX);
                mSettings.dpadOffsetY = mInitialDpadOfsY + (py - mDragStartY);
            }
            mDirty = true;
            return true;
        } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
            if (mDraggingButton >= 0) {
                float ox = 0.0f, oy = 0.0f;
                switch (mDraggingButton) {
                case 0: ox = mSettings.btnAOffsetX; oy = mSettings.btnAOffsetY; break;
                case 1: ox = mSettings.btnBOffsetX; oy = mSettings.btnBOffsetY; break;
                case 2: ox = mSettings.btnZOffsetX; oy = mSettings.btnZOffsetY; break;
                case 3: ox = mSettings.btnSpinOffsetX; oy = mSettings.btnSpinOffsetY; break;
                default: ox = mSettings.pauseOffsetX; oy = mSettings.pauseOffsetY; break;
                }
                const char* buttonName = mDraggingButton == 0 ? "A" : mDraggingButton == 1 ? "B" :
                                         mDraggingButton == 2 ? "Z" : mDraggingButton == 3 ? "SPIN" : "PAUSE";
                LOGI("layout edit: button %s now offset %.0f,%.0f",
                     buttonName, ox, oy);
            }
            mDraggingCluster = 0;
            mDraggingButton = -1;
            if (!mSavePath.empty()) mSettings.save(mSavePath);
            mDirty = true;
            return true;
        }
        return true;
    }

    // Handle Material 3 Side Sheet Drawer Touches (Tabbed & Scrollable)
    if (mSettingsOpen || mDrawerAnim > 0.05f) {
        float padX = 22.0f * uiScale;
        float contentW = drawerW - padX * 2.0f;
        float contentTop = 112.0f * uiScale;
        float contentBottom = h - 56.0f * uiScale;
        const bool showingGeneralTab = mSettingsTab == 0;
        float totalContentH = showingGeneralTab ? (1540.0f * uiScale) :
                              (mSettingsTab == 4 ? (560.0f * uiScale) :
                               (mSettingsTab == 3 ? (950.0f * uiScale) :
                               ((mPlusEdition && mSettingsTab == 1) ? (760.0f * uiScale) : (580.0f * uiScale))));
        float maxScroll = std::max(0.0f, totalContentH - (contentBottom - contentTop));

        // 1. Move Gesture for Smooth Scrolling
        if (action == AMOTION_EVENT_ACTION_MOVE && mDrawerScrollPointerId >= 0) {
            for (size_t i = 0; i < pointerCount; i++) {
                int32_t pid = AMotionEvent_getPointerId(event, i);
                if (pid == mDrawerScrollPointerId) {
                    float py = AMotionEvent_getY(event, i);
                    float deltaY = py - mDrawerTouchStartY;
                    if (fabsf(deltaY) > 6.0f * uiScale || mDrawerIsScrolling) {
                        mDrawerIsScrolling = true;
                        mDrawerScrollTarget = std::max(0.0f, std::min(mDrawerScrollStartOffset - deltaY, maxScroll));
                        mDirty = true;
                    }
                    return true;
                }
            }
        }

        // 2. Up / Pointer Up / Cancel: Finish Drag or Handle Row Tap
        if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
            int32_t liftedPid = AMotionEvent_getPointerId(event, pointerIndex);
            if (liftedPid == mDrawerScrollPointerId) {
                if (!mDrawerIsScrolling && action != AMOTION_EVENT_ACTION_CANCEL) {
                    float px = AMotionEvent_getX(event, pointerIndex);
                    float py = AMotionEvent_getY(event, pointerIndex);
                    float contentRelY = py - contentTop + mDrawerScrollY;

                    if (mSettingsTab == 0) {
                        // General Tab items
                        // Row 0: Quick Save / Quick Load Anywhere Action Buttons
                        if (contentRelY >= 0.0f && contentRelY <= 52.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                triggerSaveSlot(4, true);
                            } else {
                                triggerLoadSlot(4);
                            }
                        }
                        // Row 1: Visibility Switch
                        else if (contentRelY >= 54.0f * uiScale && contentRelY <= 102.0f * uiScale) {
                            mSettings.showControls = !mSettings.showControls;
                        }
                        // Row 2: Camera D-Pad Switch
                        else if (contentRelY >= 104.0f * uiScale && contentRelY <= 152.0f * uiScale) {
                            mSettings.showDpad = !mSettings.showDpad;
                        }
                        // Row 3: D-Pad Position Segmented
                        else if (contentRelY >= 156.0f * uiScale && contentRelY <= 208.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                mSettings.dpadOnRight = false;
                                mSettings.dpadOffsetX = 0.0f;
                                mSettings.dpadOffsetY = 0.0f;
                            } else {
                                mSettings.dpadOnRight = true;
                                mSettings.dpadOffsetX = 0.0f;
                                mSettings.dpadOffsetY = 0.0f;
                            }
                        }
                        // Row 4: Display fit and image size. Auto-fit preserves 16:9
                        // and keeps the complete game image visible on ultrawide screens.
                        else if (contentRelY >= 212.0f * uiScale && contentRelY < 260.0f * uiScale) {
                            const float sizeSelectorTop = 241.0f * uiScale;
                            if (contentRelY < sizeSelectorTop) {
                                mSettings.fullscreen = !mSettings.fullscreen;
                                showToast(mSettings.fullscreen
                                    ? "Full-screen Hor+: no stretch; wider view"
                                    : "16:9 fit: side bands are expected");
                            } else {
                                float segW = contentW / 3.0f;
                                int idx = (int)((px - (drawerX + padX)) / segW);
                                mSettings.screenScaleIndex = std::max(0, std::min(2, idx));
                                showToast(mSettings.screenScaleIndex == 0 ? "Image size: 85% (no crop)" :
                                          mSettings.screenScaleIndex == 1 ? "Image size: 95% (no crop)" :
                                          "Image size: 100% native screen");
                            }
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                        }
                        // Row 5: Full Touch Mode Switch
                        else if (contentRelY >= 262.0f * uiScale && contentRelY <= 310.0f * uiScale) {
                            mSettings.fullTouchMode = !mSettings.fullTouchMode;
                        }
                        // Row 6: Joystick Mode Segmented
                        else if (contentRelY >= 314.0f * uiScale && contentRelY <= 368.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) mSettings.dynamicStick = true;
                            else mSettings.dynamicStick = false;
                        }
                        // Row 7: Controls Opacity Segmented
                        else if (contentRelY >= 372.0f * uiScale && contentRelY <= 426.0f * uiScale) {
                            float segW = contentW / 4.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            if (idx <= 0) mSettings.opacity = 0.25f;
                            else if (idx == 1) mSettings.opacity = 0.50f;
                            else if (idx == 2) mSettings.opacity = 0.75f;
                            else mSettings.opacity = 1.00f;
                        }
                        // Row 8: Button Size Segmented
                        else if (contentRelY >= 430.0f * uiScale && contentRelY <= 484.0f * uiScale) {
                            float segW = contentW / 3.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            if (idx <= 0) mSettings.scale = 0.85f;
                            else if (idx == 1) mSettings.scale = 1.00f;
                            else mSettings.scale = 1.25f;
                        }
                        // Row 9: Layout Reposition / Reset
                        else if (contentRelY >= 488.0f * uiScale && contentRelY <= 544.0f * uiScale) {
                            float splitX = drawerX + padX + contentW * 0.56f;
                            if (px < splitX) {
                                mSettingsOpen = false;
                                mEditLayoutMode = true;
                                LOGI("layout edit: opened from the settings drawer");
                            } else {
                                clearLayoutOffsets();
                            }
                        }
                        // Row 10: Diagnostics and system logs
                        else if (contentRelY >= 548.0f * uiScale && contentRelY <= 600.0f * uiScale) {
                            mSettingsOpen = false;
                            mLogViewerOpen = true;
                            mLogFilter = 0;
                            mLogScrollY = 0.0f;
                            mLogTouchScrolling = false;
                            mLogLastRefreshMs = 0;
                            LOGI("GFX_DIAG report opened backend=%s gpu=%s fps=%.1f frameMs=%.2f cpuPct=%.1f gpuPct=%.1f ramMB=%.0f controls=%d touchOpacity=%.2f touchScale=%.2f stickDeadzone=%.2f leftStickSensitivity=%.2f rightStickSensitivity=%.2f",
                                 mActiveBackend == 1 ? "Vulkan" : "OpenGLES",
                                 mGpuRendererName.empty() ? "unknown" : mGpuRendererName.c_str(),
                                 mCurrentFps, mAvgFrameTimeMs, mCpuUsagePercent,
                                 mGpuUsagePercent, mRamUsageMB,
                                 mSettings.showControls ? 1 : 0, mSettings.opacity, mSettings.scale,
                                 mSettings.controllerDeadzone, mSettings.leftStickSensitivity,
                                 mSettings.rightStickSensitivity);
                            refreshLogCache(true);
                            mDirty = true;
                        }
                        // Row 11: Hardware Monitor HUD
                        else if (contentRelY >= 602.0f * uiScale && contentRelY < 660.0f * uiScale) {
                            float segW = contentW / 3.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            if (idx <= 0) mSettings.perfHudMode = 0; // Off
                            else if (idx == 1) mSettings.perfHudMode = 1; // FPS
                            else mSettings.perfHudMode = 2; // Full Stats
                        }
                        // Row 12: Target Frame Rate
                        else if (contentRelY >= 660.0f * uiScale && contentRelY < 718.0f * uiScale) {
                            float segW = contentW / 5.0f;
                            float relX = px - (drawerX + padX);
                            int idx = std::max(0, std::min(4, (int)(relX / segW)));
                            if (idx == 0) {
                                mSettings.targetFps = 30;
                                mSettings.batterySaver = true;
                                showToast("30 FPS + render target capped at 720p (Battery Saver)");
                            } else if (idx == 1) {
                                mSettings.targetFps = 60;
                                mSettings.batterySaver = false;
                                showToast("Target FPS: 60 FPS (Native Standard)");
                            } else if (idx == 2) {
                                mSettings.targetFps = 90;
                                mSettings.batterySaver = false;
                                showToast("Target FPS: 90 FPS (Smooth Refresh)");
                            } else if (idx == 3) {
                                mSettings.targetFps = 120;
                                mSettings.batterySaver = false;
                                showToast("Target FPS: 120 FPS (Ultra Smooth)");
                            } else {
                                mSettings.targetFps = 144;
                                mSettings.batterySaver = false;
                                showToast("Target FPS: 144 FPS (Max Refresh)");
                            }
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                        }
                        // Row 13: Graphics Backend
                        else if (contentRelY >= 718.0f * uiScale && contentRelY < 776.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                mSettings.graphicsBackend = 0; // OpenGL ES 3.2
                                showToast("Backend: OpenGL ES 3.2 (Active & Stable)");
                            } else {
                                mSettings.graphicsBackend = 1; // Vulkan 1.0
                                showToast("Backend: Vulkan 1.0 (Core Engine)");
                            }
                        }
                        // Row 14: Game Speed Multiplier (Turbo / Fast-Forward)
                        else if (contentRelY >= 776.0f * uiScale && contentRelY < 834.0f * uiScale) {
                            float segW = contentW / 5.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            mSettings.gameSpeed = std::max(1, std::min(5, idx + 1));
                            applyGameSpeed(mSettings.gameSpeed);
                            showToast(mSettings.gameSpeed == 1 ? "Game Speed: 1.0x (Normal)" :
                                      mSettings.gameSpeed == 2 ? "Game Speed: 1.5x" :
                                      mSettings.gameSpeed == 3 ? "Game Speed: 2.0x (Fast)" :
                                      mSettings.gameSpeed == 4 ? "Game Speed: 3.0x" : "Game Speed: 4.0x (Turbo)");
                        }
                        // Row 15: Game Language
                        else if (contentRelY >= 834.0f * uiScale && contentRelY < 892.0f * uiScale) {
                            float segW = contentW / 6.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            mSettings.languageChoice = std::max(0, std::min(5, idx));
                            applyLanguage(mSettings.languageChoice);
                            showToast(mSettings.languageChoice == 0 ? "Language: System Auto" :
                                      mSettings.languageChoice == 1 ? "Language: English" :
                                      mSettings.languageChoice == 2 ? "Language: Español" :
                                      mSettings.languageChoice == 3 ? "Language: Français" :
                                      mSettings.languageChoice == 4 ? "Language: Deutsch" : "Language: Italiano");
                        }
                        // Row 16: Render Resolution
                        else if (contentRelY >= 892.0f * uiScale && contentRelY < 950.0f * uiScale) {
                            float segW = contentW / 5.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            mSettings.renderScale = std::max(0, std::min(4, idx));
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                            showToast(mSettings.renderScale == 0 ? "Resolution: 480p (Max Performance)" :
                                      mSettings.renderScale == 1 ? "Resolution: 720p (HD Balanced)" :
                                      mSettings.renderScale == 2 ? "Resolution: 1080p (Full HD)" :
                                      mSettings.renderScale == 3 ? "Resolution: 1440p (2K Ultra)" :
                                      "Resolution: 4K (3840px long edge; high heat/battery use)");
                        }
                        // Row 16b: Display Refresh Rate & Framerate
                        else if (contentRelY >= 950.0f * uiScale && contentRelY < 1008.0f * uiScale) {
                            float segW = contentW / 4.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            int fps = (idx == 0) ? 60 : (idx == 1) ? 90 : (idx == 2) ? 120 : 144;
                            mSettings.targetFps = fps;
                            mSettings.batterySaver = false;
                            applyTargetFps(fps);
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                            showToast(fps == 60 ? "Framerate: 60 Hz (Standard)" :
                                      fps == 90 ? "Framerate: 90 Hz (Smooth High-Refresh)" :
                                      fps == 120 ? "Framerate: 120 Hz (Ultra Smooth Native Pacing)" :
                                      "Framerate: 144 Hz (Extreme Refresh Rate)");
                        }
                        // Row 17: Vibration Haptics
                        else if (contentRelY >= 1008.0f * uiScale && contentRelY < 1062.0f * uiScale) {
                            mSettings.haptics = !mSettings.haptics;
                            showToast(mSettings.haptics ? "Vibration Haptics: ON" : "Vibration Haptics: OFF");
                        }
                        // Row 18: Battery Saver Switch
                        else if (contentRelY >= 1062.0f * uiScale && contentRelY < 1116.0f * uiScale) {
                            mSettings.batterySaver = !mSettings.batterySaver;
                            if (mSettings.batterySaver) {
                                mSettings.targetFps = 30;
                                applyTargetFps(30);
                            } else if (mSettings.targetFps <= 30) {
                                mSettings.targetFps = 60;
                                applyTargetFps(60);
                            } else {
                                applyTargetFps(mSettings.targetFps);
                            }
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                            showToast(mSettings.batterySaver
                                ? "Battery Saver: ON (30 FPS and 720p cap; quality selection is preserved)"
                                : "Battery Saver: OFF (selected quality restored)");
                        }
                        // Row 19: Gyroscope Star Pointer Switch
                        else if (contentRelY >= 1116.0f * uiScale && contentRelY < 1170.0f * uiScale) {
                            mSettings.gyroAiming = !mSettings.gyroAiming;
                            showToast(mSettings.gyroAiming ? "Gyro Aiming: ON (Tilt Device to Aim)" : "Gyro Aiming: OFF");
                        }
                        // Row 19b: Gyro Invert X / Invert Y Buttons
                        else if (contentRelY >= 1170.0f * uiScale && contentRelY < 1220.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                mSettings.gyroInvertX = !mSettings.gyroInvertX;
                                showToast(mSettings.gyroInvertX ? "Gyro Invert Horizontal (X): ON" : "Gyro Invert Horizontal (X): OFF");
                            } else {
                                mSettings.gyroInvertY = !mSettings.gyroInvertY;
                                showToast(mSettings.gyroInvertY ? "Gyro Invert Vertical (Y): ON" : "Gyro Invert Vertical (Y): OFF");
                            }
                        }
                        // Row 19c: Relative Pointer Switch (Issue #11)
                        else if (contentRelY >= 1220.0f * uiScale && contentRelY < 1274.0f * uiScale) {
                            mSettings.relativePointer = !mSettings.relativePointer;
                            showToast(mSettings.relativePointer ? "Relative Pointer: ON (Trackpad mode)" : "Relative Pointer: OFF (Direct touch)");
                        }
                        // Row 19d: Screen Orientation Choice (Issues #18 & #19)
                        else if (contentRelY >= 1274.0f * uiScale && contentRelY < 1334.0f * uiScale) {
                            float segW = contentW / 3.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            mSettings.screenOrientationChoice = std::max(0, std::min(2, idx));
                            showToast(mSettings.screenOrientationChoice == 0 ? "Orientation: Auto Sensor (Flips 180°)" :
                                      mSettings.screenOrientationChoice == 1 ? "Orientation: Fixed Landscape" : "Orientation: Fixed Reverse Landscape");
                        }
                        // Row 20: Save Backup (Export / Import)
                        else if (contentRelY >= 1334.0f * uiScale && contentRelY < 1394.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                exportSave();
                            } else {
                                importSave();
                            }
                        }
                        // Row 22: Return to Setup / Launcher Button
                        else if (contentRelY >= 1468.0f * uiScale && contentRelY < 1548.0f * uiScale) {
                            mSettingsOpen = false;
                            port_android_exit_to_launcher();
                            return true;
                        }
                    } else if (mPlusEdition && mSettingsTab == 1) {
                        // Mod Manager Tab items
                        // Row 1: NVIDIA Shield HD Textures
                        if (contentRelY >= 48.0f * uiScale && contentRelY <= 100.0f * uiScale) {
                            mSettings.modShieldTextures = !mSettings.modShieldTextures;
                            showToast(mSettings.modShieldTextures ? "Shield HD Textures: ENABLED (1080p)" : "Shield HD Textures: DISABLED");
                        }
                        // Row 2: NVIDIA Shield 60FPS Cutscenes
                        else if (contentRelY >= 102.0f * uiScale && contentRelY <= 154.0f * uiScale) {
                            mSettings.modShieldCutscenes = !mSettings.modShieldCutscenes;
                            showToast(mSettings.modShieldCutscenes ? "Shield 60FPS Cutscenes: ENABLED" : "Shield 60FPS Cutscenes: DISABLED");
                        }
                        // Row 3: Shield Controller Curves
                        else if (contentRelY >= 156.0f * uiScale && contentRelY <= 208.0f * uiScale) {
                            mSettings.modShieldControls = !mSettings.modShieldControls;
                            showToast(mSettings.modShieldControls ? "Shield Calibrated Analog: ENABLED" : "Shield Calibrated Analog: OFF");
                        }
                        // Row 4: Classic Gamepad ABXY Prompts
                        else if (contentRelY >= 210.0f * uiScale && contentRelY <= 262.0f * uiScale) {
                            mSettings.modClassicGamepadUI = !mSettings.modClassicGamepadUI;
                            showToast(mSettings.modClassicGamepadUI ? "Gamepad ABXY Prompts: ENABLED" : "Gamepad ABXY Prompts: DISABLED");
                        }
                        // Row 5: Fly Mode Switch
                        else if (contentRelY >= 264.0f * uiScale && contentRelY <= 316.0f * uiScale) {
                            mSettings.modFly = !mSettings.modFly;
                            showToast(mSettings.modFly ? "Fly Mode: ON (Hold A/Z)" : "Fly Mode: OFF");
                        }
                        // Row 6: Cutscene Prevention Switch
                        else if (contentRelY >= 318.0f * uiScale && contentRelY <= 370.0f * uiScale) {
                            mSettings.modSkipCutscenes = !mSettings.modSkipCutscenes;
                            showToast(mSettings.modSkipCutscenes ? "Skip Cutscenes: ON" : "Skip Cutscenes: OFF");
                        }
                        // Row 7: Infinite Health Switch
                        else if (contentRelY >= 372.0f * uiScale && contentRelY <= 424.0f * uiScale) {
                            mSettings.modInfiniteHealth = !mSettings.modInfiniteHealth;
                            showToast(mSettings.modInfiniteHealth ? "Infinite Health: ON (God Mode)" : "Infinite Health: OFF");
                        }
                        // Row 8: Infinite Lives Switch
                        else if (contentRelY >= 426.0f * uiScale && contentRelY <= 478.0f * uiScale) {
                            mSettings.modInfiniteLives = !mSettings.modInfiniteLives;
                            showToast(mSettings.modInfiniteLives ? "Infinite Lives (99): ON" : "Infinite Lives: OFF");
                        }
                        // Row 9: Infinite Star Bits Switch
                        else if (contentRelY >= 480.0f * uiScale && contentRelY <= 532.0f * uiScale) {
                            mSettings.modInfiniteStarBits = !mSettings.modInfiniteStarBits;
                            showToast(mSettings.modInfiniteStarBits ? "Infinite Star Bits (999): ON" : "Infinite Star Bits: OFF");
                        }
                        // Row 10: Turbo Game Speed Multiplier
                        else if (contentRelY >= 534.0f * uiScale && contentRelY < 592.0f * uiScale) {
                            float segW = contentW / 5.0f;
                            int idx = (int)((px - (drawerX + padX)) / segW);
                            mSettings.gameSpeed = std::max(1, std::min(5, idx + 1));
                            applyGameSpeed(mSettings.gameSpeed);
                            showToast(mSettings.gameSpeed == 1 ? "Game Speed: 1.0x (Normal)" :
                                      mSettings.gameSpeed == 2 ? "Game Speed: 1.5x" :
                                      mSettings.gameSpeed == 3 ? "Game Speed: 2.0x (Fast)" :
                                      mSettings.gameSpeed == 4 ? "Game Speed: 3.0x" : "Game Speed: 4.0x (Turbo)");
                        }
                        // Row 11: Riivolution Mods Switch
                        else if (contentRelY >= 594.0f * uiScale && contentRelY <= 646.0f * uiScale) {
                            mSettings.modRiivolution = !mSettings.modRiivolution;
                            showToast(mSettings.modRiivolution ? "Riivolution Mods: ON (/sdcard/GalaxyData/mods)" : "Riivolution Mods: OFF");
                        }
                        // Row 12: Scan & Reload Mods Button
                        else if (contentRelY >= 648.0f * uiScale && contentRelY <= 704.0f * uiScale) {
                            mkdir("/sdcard/GalaxyData", 0775);
                            mkdir("/sdcard/GalaxyData/mods", 0775);
                            showToast("Scanned /sdcard/GalaxyData/mods: Active!");
                        }
                    } else if (mSettingsTab == 3) {
                        const float midX = drawerX + padX + contentW * 0.5f;
                        if (contentRelY >= 44.0f * uiScale && contentRelY < 96.0f * uiScale) {
                            mSettings.showControls = !mSettings.showControls;
                            showToast(mSettings.showControls ? "Controles tactiles activados" : "Controles tactiles ocultos");
                        } else if (contentRelY >= 98.0f * uiScale && contentRelY < 150.0f * uiScale) {
                            if (px < midX) mSettings.showButtonA = !mSettings.showButtonA;
                            else mSettings.showButtonB = !mSettings.showButtonB;
                            showToast("Visibilidad del boton actualizada");
                        } else if (contentRelY >= 152.0f * uiScale && contentRelY < 204.0f * uiScale) {
                            if (px < midX) mSettings.showButtonZ = !mSettings.showButtonZ;
                            else mSettings.showButtonSpin = !mSettings.showButtonSpin;
                            showToast("Visibilidad del boton actualizada");
                        } else if (contentRelY >= 206.0f * uiScale && contentRelY < 258.0f * uiScale) {
                            if (px < midX) mSettings.showStick = !mSettings.showStick;
                            else mSettings.showDpad = !mSettings.showDpad;
                            showToast("Visibilidad del control actualizada");
                        } else if (contentRelY >= 260.0f * uiScale && contentRelY < 312.0f * uiScale) {
                            mSettings.showPauseButton = !mSettings.showPauseButton;
                            showToast(mSettings.showPauseButton ? "Boton de pausa visible" : "Boton de pausa oculto");
                        } else if (contentRelY >= 314.0f * uiScale && contentRelY < 368.0f * uiScale) {
                            const int idx = std::max(0, std::min(2,
                                (int)((px - (drawerX + padX)) / (contentW / 3.0f))));
                            mSettings.scale = idx == 0 ? 0.85f : idx == 1 ? 1.0f : 1.25f;
                            showToast("Tamano tactil actualizado");
                        } else if (contentRelY >= 370.0f * uiScale && contentRelY < 424.0f * uiScale) {
                            const int idx = std::max(0, std::min(3,
                                (int)((px - (drawerX + padX)) / (contentW / 4.0f))));
                            mSettings.opacity = idx == 0 ? 0.25f : idx == 1 ? 0.50f : idx == 2 ? 0.75f : 1.0f;
                            showToast("Opacidad tactil actualizada");
                        } else if (contentRelY >= 426.0f * uiScale && contentRelY < 480.0f * uiScale) {
                            if (px < drawerX + padX + contentW * 0.58f) {
                                mSettingsOpen = false;
                                mEditLayoutMode = true;
                                LOGI("controls: opened touch layout editor");
                            } else {
                                clearLayoutOffsets();
                                showToast("Posiciones tactiles restauradas");
                            }
                        } else if (contentRelY >= 522.0f * uiScale && contentRelY < 576.0f * uiScale) {
                            const int idx = std::max(0, std::min(3,
                                (int)((px - (drawerX + padX)) / (contentW / 4.0f))));
                            const float values[4] = {0.10f, 0.18f, 0.25f, 0.35f};
                            mSettings.controllerDeadzone = values[idx];
                            LOGI("controller: analog deadzone set to %.2f", mSettings.controllerDeadzone);
                            showToast("Zona muerta del mando actualizada");
                        } else if (contentRelY >= 578.0f * uiScale && contentRelY < 652.0f * uiScale) {
                            const float halfW = contentW * 0.5f;
                            const float xInHalf = px < midX ? px - (drawerX + padX) : px - midX;
                            const int idx = std::max(0, std::min(3, (int)(xInHalf / (halfW / 4.0f))));
                            const float values[4] = {0.50f, 1.00f, 1.50f, 2.00f};
                            if (px < midX) {
                                mSettings.leftStickSensitivity = values[idx];
                                LOGI("controller: left stick sensitivity set to %.2f", values[idx]);
                            } else {
                                mSettings.rightStickSensitivity = values[idx];
                                port_input_set_camera_sensitivity(mSettings.rightStickSensitivity);
                                LOGI("controller: right stick sensitivity set to %.2f", values[idx]);
                            }
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                            showToast("Sensibilidad del stick actualizada");
                        } else if (contentRelY >= 658.0f * uiScale && contentRelY < 712.0f * uiScale) {
                            beginControllerCapture(px < midX ? 0 : 1);
                        } else if (contentRelY >= 714.0f * uiScale && contentRelY < 768.0f * uiScale) {
                            beginControllerCapture(px < midX ? 2 : 3);
                        } else if (contentRelY >= 770.0f * uiScale && contentRelY < 824.0f * uiScale) {
                            beginControllerCapture(px < midX ? 4 : 5);
                        } else if (contentRelY >= 826.0f * uiScale && contentRelY < 882.0f * uiScale) {
                            mSettings.controllerKeyA = -1;
                            mSettings.controllerKeyB = -1;
                            mSettings.controllerKeyC = -1;
                            mSettings.controllerKeyZ = -1;
                            mSettings.controllerKeySpin = -1;
                            mSettings.controllerKeyPause = -1;
                            mSettings.controllerDeadzone = 0.18f;
                            mSettings.leftStickSensitivity = 1.0f;
                            mSettings.rightStickSensitivity = 1.0f;
                            mSettings.cameraInvertX = true;
                            mSettings.cameraInvertY = true;
                            port_input_set_camera_sensitivity(mSettings.rightStickSensitivity);
                            port_input_set_camera_inverted(1);
                            port_input_set_camera_invert_y(1);
                            mControllerCaptureAction = -1;
                            LOGI("controller: restored standard Android key mapping and 0.18 deadzone");
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                            showToast("Mando restaurado a valores automaticos");
                        } else if (contentRelY >= 884.0f * uiScale && contentRelY < 942.0f * uiScale) {
                            if (px < midX) {
                                mSettings.cameraInvertX = !mSettings.cameraInvertX;
                                port_input_set_camera_inverted(mSettings.cameraInvertX ? 1 : 0);
                                LOGI("camera: horizontal axis inversion=%d", mSettings.cameraInvertX ? 1 : 0);
                            } else {
                                mSettings.cameraInvertY = !mSettings.cameraInvertY;
                                port_input_set_camera_invert_y(mSettings.cameraInvertY ? 1 : 0);
                                LOGI("camera: vertical axis inversion=%d", mSettings.cameraInvertY ? 1 : 0);
                            }
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            mDirty = true;
                            showToast("Inversion de camara guardada");
                        }
                    } else if (mSettingsTab == 4) {
                        if (contentRelY >= 408.0f * uiScale &&
                            contentRelY < 474.0f * uiScale) {
                            mSettings.achievementNotifications =
                                !mSettings.achievementNotifications;
                            if (!mSettings.achievementNotifications) {
                                mAchievementToastUntilMs = 0;
                            }
                            if (!mSavePath.empty()) mSettings.save(mSavePath);
                            LOGI("achievements: unlock notifications %s",
                                 mSettings.achievementNotifications ? "enabled" : "disabled");
                            showToast(mSettings.achievementNotifications
                                          ? "Avisos de logros activados"
                                          : "Avisos de logros desactivados");
                            mDirty = true;
                        } else if (contentRelY >= 480.0f * uiScale &&
                                   contentRelY < 550.0f * uiScale) {
                            mAchievementClearConfirm = true;
                            mDirty = true;
                        }
                    } else {
                        // Save tab rows share their drawing coordinates, including scroll offset.
                        // Quick actions intentionally use legacy Quick slot 4.
                        if (contentRelY >= 44.0f * uiScale && contentRelY < 100.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                triggerSaveSlot(4, true);
                            } else {
                                triggerLoadSlot(4);
                            }
                        }
                        // Row 1: Independent save-slot selector (1..5).
                        else if (contentRelY >= 112.0f * uiScale && contentRelY < 166.0f * uiScale) {
                            float segW = contentW / 5.0f;
                            int idx = std::max(0, std::min(4,
                                (int)((px - (drawerX + padX)) / segW)));
                            mSaveSlot = idx + 1;
                            mOverwriteConfirmSlot = 0;
                            mOverwriteConfirmUntilMs = 0;
                            mDirty = true;
                        }
                        // Row 2: Save selected slot.
                        else if (contentRelY >= 176.0f * uiScale && contentRelY < 228.0f * uiScale) {
                            triggerSaveSlot(mSaveSlot);
                        }
                        // Row 3: Independent load-slot selector (1..5).
                        else if (contentRelY >= 238.0f * uiScale && contentRelY < 292.0f * uiScale) {
                            float segW = contentW / 5.0f;
                            int idx = std::max(0, std::min(4,
                                (int)((px - (drawerX + padX)) / segW)));
                            mLoadSlot = idx + 1;
                            mOverwriteConfirmSlot = 0;
                            mOverwriteConfirmUntilMs = 0;
                            mDirty = true;
                        }
                        // Row 4: Load selected slot.
                        else if (contentRelY >= 300.0f * uiScale && contentRelY < 352.0f * uiScale) {
                            triggerLoadSlot(mLoadSlot);
                        }
                        // Row 5: SD card save-data export/import.
                        else if (contentRelY >= 362.0f * uiScale && contentRelY < 414.0f * uiScale) {
                            float midX = drawerX + padX + contentW * 0.5f;
                            if (px < midX) {
                                exportSave();
                            } else {
                                importSave();
                            }
                        }
                    }

                    if (!mSavePath.empty()) mSettings.save(mSavePath);
                }
                mDrawerScrollPointerId = -1;
                mDrawerIsScrolling = false;
                mDirty = true;
                return true;
            }
        }

        // 3. Down / Pointer Down: Fixed Buttons, Tabs, or Start Content Scroll
        if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);
            int32_t pid = AMotionEvent_getPointerId(event, pointerIndex);

            // Outside drawer tap (scrim) closes settings
            if (px > drawerX + drawerW) {
                mSettingsOpen = false;
                mOverwriteConfirmSlot = 0;
                mOverwriteConfirmUntilMs = 0;
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }

            // Close [X] button at top-right of drawer
            float closeBtnX = drawerX + drawerW - 36.0f * uiScale;
            float closeBtnY = 44.0f * uiScale;
            float dx = px - closeBtnX;
            float dy = py - closeBtnY;
            if (dx * dx + dy * dy <= 26.0f * uiScale * 26.0f * uiScale) {
                mSettingsOpen = false;
                mOverwriteConfirmSlot = 0;
                mOverwriteConfirmUntilMs = 0;
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }

            // Keep the same content indexes in both editions; only the tab order differs.
            if (py >= 58.0f * uiScale && py <= 104.0f * uiScale) {
                const int visibleTabCount = mPlusEdition ? 5 : 4;
                float segW = contentW / (float)visibleTabCount;
                int visibleIndex = (int)((px - (drawerX + padX)) / segW);
                visibleIndex = std::max(0, std::min(visibleTabCount - 1, visibleIndex));
                int newTab = visibleIndex;
                if (mPlusEdition) {
                    // General, Mods, Controls, Saves, Achievements -> 0, 1, 3, 2, 4.
                    if (visibleIndex == 2) newTab = 3;
                    else if (visibleIndex == 3) newTab = 2;
                } else {
                    // General, Controls, Saves, Achievements -> 0, 3, 2, 4.
                    if (visibleIndex == 1) newTab = 3;
                    else if (visibleIndex == 2) newTab = 2;
                    else if (visibleIndex == 3) newTab = 4;
                }
                if (newTab != mSettingsTab) {
                    mSettingsTab = newTab;
                    mOverwriteConfirmSlot = 0;
                    mOverwriteConfirmUntilMs = 0;
                    mDrawerScrollY = 0.0f;
                    mDrawerScrollTarget = 0.0f;
                }
                mDirty = true;
                return true;
            }

            // Fixed [ Done ] button at bottom
            if (py >= contentBottom) {
                mSettingsOpen = false;
                mOverwriteConfirmSlot = 0;
                mOverwriteConfirmUntilMs = 0;
                if (!mSavePath.empty()) mSettings.save(mSavePath);
                mDirty = true;
                return true;
            }

            // Content Viewport Touch Acquisition
            if (py >= contentTop && py < contentBottom) {
                mDrawerScrollPointerId = pid;
                mDrawerTouchStartY = py;
                mDrawerTouchStartX = px;
                mDrawerScrollStartOffset = mDrawerScrollY;
                mDrawerIsScrolling = false;
                return true;
            }

            return true;
        }
        return true;
    }

    // -------------------------------------------------------------
    // Live Virtual Controls Handling
    // -------------------------------------------------------------
    pad.connected = 1;
    uint32_t touchButtons = 0;

    // Reset button press highlights
    mBtnAPressed = false;
    mBtnBPressed = false;
    mBtnZPressed = false;
    mBtnSpinPressed = false;
    mBtnPlusPressed = false;
    mBtnDpadLeft = false;
    mBtnDpadRight = false;
    mBtnDpadUp = false;
    mBtnDpadDown = false;

    // Button layout positions - shared with the renderer and the layout editor
    ButtonLayout bl = buttonLayout(screenW, screenH);
    float aX = bl.ax, aY = bl.ay, aR = bl.ar;
    float bX = bl.bx, bY = bl.by, bR = bl.br;
    float spinX = bl.spinX, spinY = bl.spinY, spinR = bl.spinR;
    float zX = bl.zx, zY = bl.zy, zR = bl.zr;
    // Pause (+), also repositionable in the touch-layout editor.
    float plusX = bl.pauseX, plusY = bl.pauseY, plusR = bl.pauseR;

    // Camera D-Pad position
    float baseDpadX = mSettings.dpadOnRight ? (w - 140.0f * scale) : (130.0f * scale);
    float baseDpadY = mSettings.dpadOnRight ? (170.0f * scale) : (160.0f * scale);
    float dpadX = baseDpadX + mSettings.dpadOffsetX;
    float dpadY = baseDpadY + mSettings.dpadOffsetY;
    float dpadTotalR = 58.0f * scale;

    // Stick default position - Bottom left thumb area
    float defaultStickX = 175.0f * scale + mSettings.stickOffsetX;
    float defaultStickY = h - 175.0f * scale + mSettings.stickOffsetY;
    float stickBaseR = 95.0f * scale;
    float stickMaxR = 100.0f * scale;

    // 1. Complete release: when all fingers are lifted or gesture canceled
    if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) {
        for (int i = 0; i < 10; i++) {
            mTouches[i] = {};
        }
        mStickPointerId = -1;
        mStickKnobX = mStickBaseX;
        mStickKnobY = mStickBaseY;
        mTouchStickX = 0.0f;
        mTouchStickY = 0.0f;
        pad.stickX = 0.0f;
        pad.stickY = 0.0f;
        touchButtons = 0;
        pad.buttons &= ~0x3C1F; // Only clear touch bits, preserve physical controller buttons
        mFingerDownOnA = false;
        mFingerDownOnB = false;
        mFingerDownOnZ = false;
        mFingerDownOnSpin = false;
        mFingerDownOnPlus = false;
        mBtnAPressed = mBtnBPressed = mBtnZPressed = mBtnSpinPressed = mBtnPlusPressed = false;
        mBtnDpadLeft = mBtnDpadRight = mBtnDpadUp = mBtnDpadDown = false;
        mDirty = true;

        if (mTouchPointerId >= 0) {
            int64_t now = currentTimeMs();
            int64_t dur = now - mTouchStartTime;
            float px = AMotionEvent_getX(event, pointerIndex);
            float py = AMotionEvent_getY(event, pointerIndex);
            float fdx = px - mTouchStartX;
            float fdy = py - mTouchStartY;
            bool isTitle = (port_is_title_screen() != 0);
            if (isTitle && dur < 350 && (fdx * fdx + fdy * fdy <= 45.0f * uiScale * 45.0f * uiScale)) {
                mTapDelayFrames = 0;
                mTapPendingFrames = 6;
            }
            mTouchPointerId = -1;
        }

        pad.pointerValid = 1;
        pad.pointerX = mPointerX;
        pad.pointerY = mPointerY;
        pad.pointerDist = 2.0f;
        return true;
    }

    // 2. Single finger release during multi-touch
    if (action == AMOTION_EVENT_ACTION_POINTER_UP) {
        int32_t liftedPid = AMotionEvent_getPointerId(event, pointerIndex);
        for (int i = 0; i < 10; i++) {
            if (mTouches[i].id == liftedPid) {
                if (mTouches[i].role == TouchRole::STICK) {
                    mStickPointerId = -1;
                    mStickKnobX = mStickBaseX;
                    mStickKnobY = mStickBaseY;
                    mTouchStickX = 0.0f;
                    mTouchStickY = 0.0f;
                    pad.stickX = 0.0f;
                    pad.stickY = 0.0f;
                }
                mTouches[i] = {};
            }
        }
        if (liftedPid == mTouchPointerId) {
            bool isTitle = (port_is_title_screen() != 0);
            if (isTitle) {
                int64_t now = currentTimeMs();
                int64_t dur = now - mTouchStartTime;
                float px = AMotionEvent_getX(event, pointerIndex);
                float py = AMotionEvent_getY(event, pointerIndex);
                float fdx = px - mTouchStartX;
                float fdy = py - mTouchStartY;
                if (dur < 350 && (fdx * fdx + fdy * fdy <= 45.0f * uiScale * 45.0f * uiScale)) {
                    mTapPendingFrames = 6;
                }
            }
            mTouchPointerId = -1;
        }
        mDirty = true;
    }

    // 3. New Touch Down (Assign dedicated role to this finger)
    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        float px = AMotionEvent_getX(event, pointerIndex);
        float py = AMotionEvent_getY(event, pointerIndex);
        int32_t pid = AMotionEvent_getPointerId(event, pointerIndex);

        int slot = -1;
        for (int i = 0; i < 10; i++) {
            if (mTouches[i].id == pid) { slot = i; break; }
            if (slot < 0 && mTouches[i].id < 0) { slot = i; }
        }
        if (slot >= 0) {
            mTouches[slot].id = pid;
            mTouches[slot].startX = px;
            mTouches[slot].startY = py;
            mTouches[slot].startTime = currentTimeMs();
            mTouches[slot].role = TouchRole::NONE;

            // Stick acquisition: bottom-left thumb zone
            bool stickEligible = false;
            if (mSettings.showControls && !mGamepadConnected &&
                mSettings.showStick && mStickPointerId < 0) {
                if (mSettings.dynamicStick) {
                    stickEligible = (px <= w * 0.38f && py >= h * 0.45f);
                    } else {
                    float d2 = (px - defaultStickX) * (px - defaultStickX) + (py - defaultStickY) * (py - defaultStickY);
                    stickEligible = (d2 <= (stickBaseR * 1.5f) * (stickBaseR * 1.5f));
                }
            }

            if (stickEligible) {
                mTouches[slot].role = TouchRole::STICK;
                mStickPointerId = pid;
                if (mSettings.dynamicStick) {
                    mStickBaseX = px;
                    mStickBaseY = py;
                } else {
                    mStickBaseX = defaultStickX;
                    mStickBaseY = defaultStickY;
                }
                mStickKnobX = px;
                mStickKnobY = py;
                mDirty = true;
            }
            // Camera D-Pad
            else if (mSettings.showControls && !mGamepadConnected && mSettings.showDpad &&
                     ((px - dpadX)*(px - dpadX) + (py - dpadY)*(py - dpadY) <= dpadTotalR * dpadTotalR)) {
                mTouches[slot].role = TouchRole::DPAD;
                mDirty = true;
            }
            // Action Buttons - NEAREST EXCLUSIVE MATCHING
            else if (mSettings.showControls && !mGamepadConnected) {
                struct BtnCandidate {
                    TouchRole role;
                    float distSq;
                    float maxDistSq;
                    bool visible;
                };
                BtnCandidate candidates[5] = {
                    {TouchRole::BTN_A, (px - aX)*(px - aX) + (py - aY)*(py - aY), (aR * 1.15f) * (aR * 1.15f), mSettings.showButtonA},
                    {TouchRole::BTN_B, (px - bX)*(px - bX) + (py - bY)*(py - bY), (bR * 1.15f) * (bR * 1.15f), mSettings.showButtonB},
                    {TouchRole::BTN_Z, (px - zX)*(px - zX) + (py - zY)*(py - zY), (zR * 1.15f) * (zR * 1.15f), mSettings.showButtonZ},
                    {TouchRole::BTN_SPIN, (px - spinX)*(px - spinX) + (py - spinY)*(py - spinY), (spinR * 1.15f) * (spinR * 1.15f), mSettings.showButtonSpin},
                    {TouchRole::BTN_PLUS, (px - plusX)*(px - plusX) + (py - plusY)*(py - plusY), (plusR * 1.25f) * (plusR * 1.25f), mSettings.showPauseButton},
                };

                int bestIdx = -1;
                float bestNormDist = 1.0f;
                for (int b = 0; b < 5; b++) {
                    if (!candidates[b].visible) continue;
                    float normDist = candidates[b].distSq / candidates[b].maxDistSq;
                    if (normDist <= 1.0f && normDist < bestNormDist) {
                        bestNormDist = normDist;
                        bestIdx = b;
                    }
                }

                if (bestIdx >= 0) {
                    mTouches[slot].role = candidates[bestIdx].role;
                    mDirty = true;
                } else {
                    mTouches[slot].role = TouchRole::STAR_POINTER;
                }
            } else {
                mTouches[slot].role = TouchRole::STAR_POINTER;
            }

            if (mTouches[slot].role == TouchRole::STAR_POINTER) {
                if (mTouchPointerId < 0) {
                    mTouchPointerId = pid;
                    mTouchStartTime = currentTimeMs();
                    mTouchStartX = px;
                    mTouchStartY = py;
                }
                mTouches[slot].lastX = px;
                mTouches[slot].lastY = py;
                if (!mSettings.relativePointer) {
                    if (vpW > 0 && vpH > 0) {
                        setPointerPos(((px - vpX) / (float)vpW) * 2.0f - 1.0f,
                                      ((py - vpY) / (float)vpH) * 2.0f - 1.0f);
                    }
                }
            }
        }
    }

    // 4. Update state from active pointers
    mFingerDownOnA = false;
    mFingerDownOnB = false;
    mFingerDownOnZ = false;
    mFingerDownOnSpin = false;
    mFingerDownOnPlus = false;
    mBtnDpadLeft = false;
    mBtnDpadRight = false;
    mBtnDpadUp = false;
    mBtnDpadDown = false;
    bool stickFound = false;

    for (size_t i = 0; i < pointerCount; i++) {
        if (action == AMOTION_EVENT_ACTION_POINTER_UP && (int32_t)i == pointerIndex) {
            continue;
        }

        int32_t pid = AMotionEvent_getPointerId(event, i);
        float px = AMotionEvent_getX(event, i);
        float py = AMotionEvent_getY(event, i);

        TouchRole role = TouchRole::NONE;
        int touchSlot = -1;
        for (int s = 0; s < 10; s++) {
            if (mTouches[s].id == pid) {
                role = mTouches[s].role;
                touchSlot = s;
                break;
            }
        }

        if (role == TouchRole::STICK) {
            stickFound = true;
            float dx = px - mStickBaseX;
            float dy = py - mStickBaseY;
            float dist = sqrtf(dx * dx + dy * dy);
            float deadzone = 16.0f * scale;

            if (dist > stickMaxR && dist > 0.001f) {
                mStickKnobX = mStickBaseX + (dx / dist) * stickMaxR;
                mStickKnobY = mStickBaseY + (dy / dist) * stickMaxR;
                mTouchStickX = dx / dist;
                mTouchStickY = -dy / dist;
            } else if (dist > deadzone) {
                mStickKnobX = px;
                mStickKnobY = py;
                float norm = (dist - deadzone) / (stickMaxR - deadzone);
                mTouchStickX = (dx / dist) * norm;
                mTouchStickY = -(dy / dist) * norm;
            } else {
                mStickKnobX = px;
                mStickKnobY = py;
                mTouchStickX = 0.0f;
                mTouchStickY = 0.0f;
            }
            const float sensitivity = std::max(0.25f, std::min(2.0f, mSettings.leftStickSensitivity));
            mTouchStickX = std::max(-1.0f, std::min(1.0f, mTouchStickX * sensitivity));
            mTouchStickY = std::max(-1.0f, std::min(1.0f, mTouchStickY * sensitivity));
            pad.stickX = mTouchStickX;
            pad.stickY = mTouchStickY;
        }
        else if (role == TouchRole::DPAD) {
            float dDpadX = px - dpadX;
            float dDpadY = py - dpadY;
            float distDpad = sqrtf(dDpadX * dDpadX + dDpadY * dDpadY);
            float deadzone = 8.0f * scale;
            if (distDpad > deadzone && distDpad <= dpadTotalR * 1.35f) {
                if (dDpadX < -deadzone && fabsf(dDpadX) > fabsf(dDpadY) * 0.40f) {
                    touchButtons |= 0x0001; // WPAD_BUTTON_LEFT
                    mBtnDpadLeft = true;
                } else if (dDpadX > deadzone && fabsf(dDpadX) > fabsf(dDpadY) * 0.40f) {
                    touchButtons |= 0x0002; // WPAD_BUTTON_RIGHT
                    mBtnDpadRight = true;
                }
                if (dDpadY < -deadzone && fabsf(dDpadY) > fabsf(dDpadX) * 0.40f) {
                    touchButtons |= 0x0008; // WPAD_BUTTON_UP
                    mBtnDpadUp = true;
                } else if (dDpadY > deadzone && fabsf(dDpadY) > fabsf(dDpadX) * 0.40f) {
                    touchButtons |= 0x0004; // WPAD_BUTTON_DOWN
                    mBtnDpadDown = true;
                }
            }
        }
        else if (role == TouchRole::BTN_A) {
            touchButtons |= 0x0800; // WPAD_BUTTON_A
            if (!mBtnAPressed && mSettings.haptics) port_android_vibrate(22);
            mBtnAPressed = true;
            mFingerDownOnA = true;
        }
        else if (role == TouchRole::BTN_B) {
            touchButtons |= 0x0400; // WPAD_BUTTON_B
            if (!mBtnBPressed && mSettings.haptics) port_android_vibrate(18);
            mBtnBPressed = true;
            mFingerDownOnB = true;
        }
        else if (role == TouchRole::BTN_Z) {
            touchButtons |= 0x2000; // WPAD_BUTTON_Z
            if (!mBtnZPressed && mSettings.haptics) port_android_vibrate(20);
            mBtnZPressed = true;
            mFingerDownOnZ = true;
        }
        else if (role == TouchRole::BTN_SPIN) {
            spinFrames = 6;
            if (!mBtnSpinPressed && mSettings.haptics) port_android_vibrate(35);
            mBtnSpinPressed = true;
            mFingerDownOnSpin = true;
        }
        else if (role == TouchRole::BTN_PLUS) {
            touchButtons |= 0x0010; // WPAD_BUTTON_PLUS
            if (!mBtnPlusPressed && mSettings.haptics) port_android_vibrate(25);
            mBtnPlusPressed = true;
            mFingerDownOnPlus = true;
        }
        else {
            // Star Pointer
            if (vpW > 0 && vpH > 0) {
                if (mSettings.relativePointer) {
                    if (touchSlot >= 0) {
                        float dx = ((px - mTouches[touchSlot].lastX) / (float)vpW) * 2.0f;
                        float dy = ((py - mTouches[touchSlot].lastY) / (float)vpH) * 2.0f;
                        setPointerPos(mPointerX + dx, mPointerY + dy);
                        mTouches[touchSlot].lastX = px;
                        mTouches[touchSlot].lastY = py;
                    }
                } else {
                    setPointerPos(((px - vpX) / (float)vpW) * 2.0f - 1.0f,
                                  ((py - vpY) / (float)vpH) * 2.0f - 1.0f);
                    if (touchSlot >= 0) {
                        mTouches[touchSlot].lastX = px;
                        mTouches[touchSlot].lastY = py;
                    }
                }
            }
        }
    }

    if (!stickFound) {
        if (mStickPointerId >= 0) {
            mStickPointerId = -1;
            mStickKnobX = mStickBaseX;
            mStickKnobY = mStickBaseY;
            mDirty = true;
        }
        mTouchStickX = 0.0f;
        mTouchStickY = 0.0f;
        pad.stickX = 0.0f;
        pad.stickY = 0.0f;
    }

    if (mTapPendingFrames > 0) {
        bool isTitle = (port_is_title_screen() != 0);
        if (isTitle) {
            touchButtons |= 0x0C00; // WPAD_BUTTON_A | WPAD_BUTTON_B
            mBtnBPressed = true;
            mBtnAPressed = true;
        }
    }

    pad.buttons = (pad.buttons & ~0x3C1F) | touchButtons;
    pad.pointerValid = 1;
    pad.pointerX = mPointerX;
    pad.pointerY = mPointerY;
    pad.pointerDist = 2.0f;

    return true;
}

void UIOverlay::generateVertices(int screenW, int screenH, std::vector<UIVertex>& verts) {
    verts.clear();
    verts.reserve(1024);

    float baseDpi = (float)screenH / 650.0f;
    float uiScale = baseDpi;
    float scale = baseDpi * mSettings.scale;
    float op = mSettings.opacity;
    float w = (float)screenW;
    float h = (float)screenH;

    // Animation progress calculation
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    if (mLastAnimTime == 0) mLastAnimTime = now;
    float dt = (float)(now - mLastAnimTime) / 1e9f;
    mLastAnimTime = now;
    if (dt < 0.0001f) dt = 0.016f;
    if (dt > 0.1f) dt = 0.1f;

    float target = mSettingsOpen ? 1.0f : 0.0f;
    if (fabsf(mDrawerAnim - target) > 0.0005f) {
        float speed = 14.0f; // Material 3 snappy deceleration
        mDrawerAnim += (target - mDrawerAnim) * (1.0f - expf(-speed * dt));
        if (fabsf(mDrawerAnim - target) < 0.001f) {
            mDrawerAnim = target;
        }
        mDirty = true;
    }

    // Match the input geometry above so the same touch targets remain aligned.
    float drawerW = settingsDrawerWidth(w, h, uiScale);
    float drawerX = (mDrawerAnim - 1.0f) * drawerW;
    float cogRot = mDrawerAnim * 1.5707963f; // Smooth 90-degree cogwheel turn

    auto addQuad = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                       const Color& c, float mode, float param, float aspect = 1.0f, float extra = 0.0f) {
        verts.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x1, y0, u1, v0, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a, mode, param, aspect, extra});

        verts.push_back({x0, y0, u0, v0, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
        verts.push_back({x0, y1, u0, v1, c.r, c.g, c.b, c.a, mode, param, aspect, extra});
    };

    auto drawCircle = [&](float cx, float cy, float r, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 1.0f, 0.0f);
    };

    auto drawRing = [&](float cx, float cy, float r, float innerRatio, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 1.0f, innerRatio);
    };

    auto drawRoundRect = [&](float x0, float y0, float x1, float y1, float radius, const Color& c) {
        float rw = x1 - x0;
        float rh = y1 - y0;
        if (rw <= 0.0f || rh <= 0.0f) return;
        if (rw >= rh) {
            float aspect = rw / rh;
            float rNorm = std::min(1.0f, radius / (rh * 0.5f));
            addQuad(x0, y0, x1, y1, -aspect, -1.0f, aspect, 1.0f, c, 0.0f, rNorm, aspect, 0.0f);
        } else {
            float aspect = rh / rw;
            float rNorm = std::min(1.0f, radius / (rw * 0.5f));
            addQuad(x0, y0, x1, y1, -1.0f, -aspect, 1.0f, aspect, c, 0.0f, rNorm, aspect, 1.0f);
        }
    };

    auto drawCogwheel = [&](float cx, float cy, float r, const Color& c, float rot = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 3.0f, rot);
    };

    auto drawCloseIcon = [&](float cx, float cy, float r, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 4.0f, 0.0f);
    };

    auto drawCyclone = [&](float cx, float cy, float r, const Color& c, float rot = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 5.0f, rot);
    };

    auto drawChevron = [&](float cx, float cy, float r, const Color& c, float rotAngle = 0.0f) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 6.0f, rotAngle);
    };

    auto drawCameraIcon = [&](float cx, float cy, float r, const Color& c) {
        addQuad(cx - r, cy - r, cx + r, cy + r, -1.0f, -1.0f, 1.0f, 1.0f, c, 7.0f, 0.0f);
    };

    auto drawLine = [&](float x0, float y0, float x1, float y1, float thickness, const Color& c) {
        float dx = x1 - x0;
        float dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;
        float hThick = thickness * 0.5f;
        float nx = -dy / len * hThick;
        float ny = dx / len * hThick;

        verts.push_back({x0 + nx, y0 + ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1 + nx, y1 + ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1 - nx, y1 - ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});

        verts.push_back({x0 + nx, y0 + ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1 - nx, y1 - ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x0 - nx, y0 - ny, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
    };

    auto drawTriangle = [&](float x0, float y0, float x1, float y1, float x2, float y2, const Color& c) {
        verts.push_back({x0, y0, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x1, y1, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
        verts.push_back({x2, y2, 0.0f, 0.0f, c.r, c.g, c.b, c.a, 0.0f, 0.0f, 1.0f, 0.0f});
    };

    const ui_font::Face& face = ui_font::kFaces[0];
    auto measureText = [&](const char* text, float sizeScale) -> float {
        float strW = 0.0f;
        for (const char* p = text; *p; p++) {
            char ch = *p;
            if (ch >= 32 && ch < 127) {
                strW += (face.glyphs[ch - 32].advance * 0.25f) * sizeScale;
            }
        }
        return strW;
    };

    auto drawText = [&](const char* text, float x, float baseline, float sizeScale, const Color& c, int align = 0) {
        float strW = measureText(text, sizeScale);
        float curX = x;
        if (align == 1) curX -= strW * 0.5f;      // Center
        else if (align == 2) curX -= strW;        // Right

        for (const char* p = text; *p; p++) {
            char ch = *p;
            if (ch < 32 || ch >= 127) continue;
            const ui_font::Glyph& g = face.glyphs[ch - 32];
            float gx = curX + g.left * sizeScale;
            float gy = baseline - g.top * sizeScale;
            float gw = g.w * sizeScale;
            float gh = g.h * sizeScale;
            float u0 = (float)g.x / (float)face.width;
            float v0 = (float)g.y / (float)face.height;
            float u1 = (float)(g.x + g.w) / (float)face.width;
            float v1 = (float)(g.y + g.h) / (float)face.height;

            addQuad(gx, gy, gx + gw, gy + gh, u0, v0, u1, v1, c, 2.0f, 0.0f);
            curX += (g.advance * 0.25f) * sizeScale;
        }
    };

    auto drawM3Switch = [&](float x, float y, float swW, float swH, bool on) {
        float r = swH * 0.5f;
        if (on) {
            // Track: Primary Container (#0B57D0)
            drawRoundRect(x, y, x + swW, y + swH, r, rgba(0.043f, 0.341f, 0.816f, 1.0f));
            // Thumb: High-contrast white disc at right
            float thumbR = r * 0.70f;
            drawCircle(x + swW - r, y + r, thumbR, rgba(1.0f, 1.0f, 1.0f, 1.0f));
        } else {
            // Track: Surface Container Highest (#303239) with subtle outline
            drawRoundRect(x, y, x + swW, y + swH, r, rgba(0.188f, 0.196f, 0.224f, 0.95f));
            drawRoundRect(x + 1.0f, y + 1.0f, x + swW - 1.0f, y + swH - 1.0f, r - 1.0f, rgba(0.267f, 0.278f, 0.310f, 0.60f));
            // Thumb: Outline disc at left
            float thumbR = r * 0.56f;
            drawCircle(x + r, y + r, thumbR, rgba(0.60f, 0.64f, 0.72f, 1.0f));
        }
    };

    auto drawSegmentedGroup = [&](float x, float y, float grpW, float grpH, const char** labels, int count, int activeIdx) {
        float r = 12.0f * uiScale;
        // Group container: Surface Container High (#26282E) with outline
        drawRoundRect(x, y, x + grpW, y + grpH, r, rgba(0.149f, 0.157f, 0.180f, 0.95f));
        drawRoundRect(x, y, x + grpW, y + grpH, r, rgba(0.267f, 0.278f, 0.310f, 0.50f));

        float segW = grpW / (float)count;
        for (int i = 0; i < count; i++) {
            float sx0 = x + i * segW;
            float sx1 = sx0 + segW;
            bool active = (i == activeIdx);
            if (active) {
                // Active pill: Primary Container (#0B57D0)
                drawRoundRect(sx0 + 2.0f, y + 2.0f, sx1 - 2.0f, y + grpH - 2.0f, r - 2.0f, rgba(0.043f, 0.341f, 0.816f, 1.0f));
                drawText(labels[i], (sx0 + sx1) * 0.5f, y + grpH * 0.64f, 0.36f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);
            } else {
                drawText(labels[i], (sx0 + sx1) * 0.5f, y + grpH * 0.64f, 0.36f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.95f), 1);
                if (i > 0 && (i - 1) != activeIdx) {
                    drawRoundRect(sx0 - 0.5f, y + 6.0f, sx0 + 0.5f, y + grpH - 6.0f, 0.0f, rgba(0.267f, 0.278f, 0.310f, 0.40f));
                }
            }
        }
    };

    // -------------------------------------------------------------
    // Draw On-Screen Controls (HUD) - Modern Minimalist Frosted Theme
    // -------------------------------------------------------------
    if (mSettings.showControls && !mGamepadConnected && mDrawerAnim < 0.98f) {
        float hudOp = op * (1.0f - mDrawerAnim * 0.75f);

        // ---------------------------------------------------------
        // 1. Virtual Joystick - Precision Minimalist Reticle & Laser Tether
        // ---------------------------------------------------------
        if (mSettings.showStick) {
        float defaultStickX = 175.0f * scale + mSettings.stickOffsetX;
        float defaultStickY = h - 175.0f * scale + mSettings.stickOffsetY;
        float baseX = (mStickPointerId >= 0) ? mStickBaseX : defaultStickX;
        float baseY = (mStickPointerId >= 0) ? mStickBaseY : defaultStickY;
        float knobX = (mStickPointerId >= 0) ? mStickKnobX : baseX;
        float knobY = (mStickPointerId >= 0) ? mStickKnobY : baseY;
        float baseR = 95.0f * scale;
        float knobR = 38.0f * scale;
        bool stickActive = (mStickPointerId >= 0);

        // Base Plate: Translucent dark glass disc
        drawCircle(baseX, baseY, baseR, rgba(0.05f, 0.07f, 0.11f, hudOp * 0.28f));

        // Outer Reticle: Razor-thin anti-aliased ring
        drawRing(baseX, baseY, baseR, 0.96f, rgba(0.60f, 0.80f, 1.0f, hudOp * 0.45f));

        // Cardinal Precision Orientation Ticks (N, S, E, W)
        float tickIn = baseR * 0.84f;
        float tickOut = baseR * 0.95f;
        float tickW = 2.0f * scale;
        Color tickCol = rgba(0.75f, 0.88f, 1.0f, hudOp * 0.50f);
        drawLine(baseX, baseY - tickOut, baseX, baseY - tickIn, tickW, tickCol); // N
        drawLine(baseX, baseY + tickIn, baseX, baseY + tickOut, tickW, tickCol); // S
        drawLine(baseX - tickOut, baseY, baseX - tickIn, baseY, tickW, tickCol); // W
        drawLine(baseX + tickIn, baseY, baseX + tickOut, baseY, tickW, tickCol); // E

        // Deadzone Center Target: Minimalist ring & center micro-dot
        drawRing(baseX, baseY, 13.0f * scale, 0.86f, rgba(0.50f, 0.70f, 1.0f, hudOp * 0.35f));
        drawCircle(baseX, baseY, 2.5f * scale, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.60f));

        // Dynamic Luminous Laser Tether (Center to Knob deflection vector)
        float sDx = knobX - baseX;
        float sDy = knobY - baseY;
        float sDist = sqrtf(sDx * sDx + sDy * sDy);
        if (sDist > 5.0f * scale) {
            // Soft cyan outer glow tether
            drawLine(baseX, baseY, knobX, knobY, 3.2f * scale, rgba(0.22f, 0.74f, 0.97f, hudOp * 0.65f));
            // Crisp white core laser beam
            drawLine(baseX, baseY, knobX, knobY, 1.4f * scale, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.85f));
        }

        // Floating Frosted Thumb Puck
        float kpR = stickActive ? (knobR * 1.04f) : knobR;
        // Knob base disc
        drawCircle(knobX, knobY, kpR, rgba(0.09f, 0.13f, 0.22f, hudOp * (stickActive ? 0.85f : 0.65f)));
        // Outer glowing rim
        Color knobRim = stickActive ? rgba(0.22f, 0.74f, 0.97f, hudOp * 0.95f) : rgba(0.60f, 0.80f, 1.0f, hudOp * 0.65f);
        drawRing(knobX, knobY, kpR, 0.90f, knobRim);
        // Inner concentric thumb groove ring
        drawRing(knobX, knobY, kpR * 0.68f, 0.93f, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.35f));
        // Center tactile micro-dot
        drawCircle(knobX, knobY, 3.0f * scale, stickActive ? rgba(0.22f, 0.74f, 0.97f, hudOp * 0.95f) : rgba(1.0f, 1.0f, 1.0f, hudOp * 0.50f));
        }

        // ---------------------------------------------------------
        // 2. Action Buttons - Frosted Glass with Signature Micro-Accents
        // ---------------------------------------------------------
        float btnBaseX = w - 150.0f * scale + mSettings.buttonOffsetX;
        float btnBaseY = h - 150.0f * scale + mSettings.buttonOffsetY;
        ButtonLayout bl = buttonLayout(screenW, screenH);

        auto drawModernButton = [&](float cx, float cy, float r, const char* label, uint32_t accentHex, bool pressed, int specialGlyph = 0) {
            float pr = pressed ? (r * 0.93f) : r;
            float baseAlpha = pressed ? (hudOp * 0.88f) : (hudOp * 0.36f);
            float borderAlpha = pressed ? (hudOp * 0.98f) : (hudOp * 0.65f);
            float ringAlpha = pressed ? (hudOp * 0.85f) : (hudOp * 0.25f);

            // Frosted dark glass base plate
            Color baseBg = pressed ? hexColor(accentHex, baseAlpha) : rgba(0.06f, 0.08f, 0.13f, baseAlpha);
            drawCircle(cx, cy, pr, baseBg);

            // Thin luminous perimeter ring (1.5px)
            Color outerRing = pressed ? rgba(1.0f, 1.0f, 1.0f, borderAlpha) : hexColor(accentHex, borderAlpha);
            drawRing(cx, cy, pr, 0.92f, outerRing);

            // Inner concentric frosted micro-ring
            drawRing(cx, cy, pr * 0.80f, 0.95f, rgba(1.0f, 1.0f, 1.0f, ringAlpha));

            // High-contrast clean glyph / typography
            Color glyphCol = pressed ? rgba(1.0f, 1.0f, 1.0f, hudOp * 1.0f) : rgba(0.92f, 0.95f, 1.0f, hudOp * 0.92f);

            if (specialGlyph == 1) { // SPIN: Procedural Cyclone glyph + clean micro-label
                float iconR = pr * 0.44f;
                drawCyclone(cx, cy - 5.0f * scale, iconR, glyphCol, 0.0f);
                drawText("SPIN", cx, cy + pr * 0.58f, 0.26f * scale, glyphCol, 1);
            } else if (specialGlyph == 2) { // PLUS (+): Modern precision cross
                float armLen = pr * 0.38f;
                float thick = 2.4f * scale;
                drawLine(cx - armLen, cy, cx + armLen, cy, thick, glyphCol);
                drawLine(cx, cy - armLen, cx, cy + armLen, thick, glyphCol);
            } else { // Standard typography (A, B, Z)
                float textSize = (r > 48.0f * scale) ? (0.64f * scale) : (0.54f * scale);
                drawText(label, cx, cy + 11.0f * scale, textSize, glyphCol, 1);
            }
        };

        // Button A (Jump) - Luminous Electric Cyan (#38BDF8)
        if (mSettings.showButtonA) drawModernButton(bl.ax, bl.ay, bl.ar, "A", 0x38BDF8, mBtnAPressed);

        // Button B (Star Bit Shoot) - Luminous Star Amber (#FBBF24)
        if (mSettings.showButtonB) drawModernButton(bl.bx, bl.by, bl.br, "B", 0xFBBF24, mBtnBPressed);

        // Button SPIN - Luminous Emerald Mint (#34D399) with Cyclone Glyph
        if (mSettings.showButtonSpin) drawModernButton(bl.spinX, bl.spinY, bl.spinR, "SPIN", 0x34D399, mBtnSpinPressed, 1);

        // Button Z (Crouch) - Luminous Lavender Violet (#A78BFA)
        if (mSettings.showButtonZ) drawModernButton(bl.zx, bl.zy, bl.zr, "Z", 0xA78BFA, mBtnZPressed);

        // Button + (Pause) - Minimalist Pearl Slate (#E2E8F0) with Vector Cross
        if (mSettings.showPauseButton) drawModernButton(bl.pauseX, bl.pauseY, bl.pauseR, "+", 0x94A3B8, mBtnPlusPressed, 2);

        // ---------------------------------------------------------
        // 3. Camera D-Pad - Directional Precision Disc with Chevrons & Camera Icon
        // ---------------------------------------------------------
        if (mSettings.showDpad) {
            float baseDpadX = mSettings.dpadOnRight ? (w - 140.0f * scale) : (130.0f * scale);
            float baseDpadY = mSettings.dpadOnRight ? (170.0f * scale) : (160.0f * scale);
            float dpadX = baseDpadX + mSettings.dpadOffsetX;
            float dpadY = baseDpadY + mSettings.dpadOffsetY;
            float dialR = 56.0f * scale;
            float armDist = 36.0f * scale;
            float chevronR = 14.0f * scale;

            // Frosted circular dial plate
            drawCircle(dpadX, dpadY, dialR, rgba(0.06f, 0.08f, 0.13f, hudOp * 0.32f));

            // Outer razor-thin precision ring
            drawRing(dpadX, dpadY, dialR, 0.95f, rgba(0.55f, 0.75f, 1.0f, hudOp * 0.42f));

            // Quadrant division hairlines (45 deg diagonals)
            float divInner = 20.0f * scale;
            float divOuter = dialR * 0.88f;
            float diag = 0.7071f;
            Color divCol = rgba(1.0f, 1.0f, 1.0f, hudOp * 0.16f);
            drawLine(dpadX + divInner * diag, dpadY + divInner * diag, dpadX + divOuter * diag, dpadY + divOuter * diag, 1.4f * scale, divCol);
            drawLine(dpadX - divInner * diag, dpadY + divInner * diag, dpadX - divOuter * diag, dpadY + divOuter * diag, 1.4f * scale, divCol);
            drawLine(dpadX + divInner * diag, dpadY - divInner * diag, dpadX + divOuter * diag, dpadY - divOuter * diag, 1.4f * scale, divCol);
            drawLine(dpadX - divInner * diag, dpadY - divInner * diag, dpadX - divOuter * diag, dpadY - divOuter * diag, 1.4f * scale, divCol);

            // 4 Directional Chevron Sectors (Left, Right, Up, Down)
            auto drawDpadSector = [&](float cx, float cy, float rotAngle, bool pressed) {
                if (pressed) {
                    drawCircle(cx, cy, 20.0f * scale, rgba(0.22f, 0.74f, 0.97f, hudOp * 0.80f));
                    drawRing(cx, cy, 20.0f * scale, 0.88f, rgba(1.0f, 1.0f, 1.0f, hudOp * 0.95f));
                }
                Color chevCol = pressed ? rgba(1.0f, 1.0f, 1.0f, hudOp * 1.0f) : rgba(0.85f, 0.92f, 1.0f, hudOp * 0.75f);
                float chevSize = pressed ? (chevronR * 1.15f) : chevronR;
                drawChevron(cx, cy, chevSize, chevCol, rotAngle);
            };

            // Left (rot = PI)
            drawDpadSector(dpadX - armDist, dpadY, 3.14159265f, mBtnDpadLeft);
            // Right (rot = 0)
            drawDpadSector(dpadX + armDist, dpadY, 0.0f, mBtnDpadRight);
            // Up (rot = -PI/2)
            drawDpadSector(dpadX, dpadY - armDist, -1.5707963f, mBtnDpadUp);
            // Down (rot = PI/2)
            drawDpadSector(dpadX, dpadY + armDist, 1.5707963f, mBtnDpadDown);

            // Center Concave Hub with Modern Camera Icon
            float hubR = 18.0f * scale;
            drawCircle(dpadX, dpadY, hubR, rgba(0.09f, 0.13f, 0.22f, hudOp * 0.80f));
            drawRing(dpadX, dpadY, hubR, 0.88f, rgba(0.55f, 0.75f, 1.0f, hudOp * 0.55f));
            drawCameraIcon(dpadX, dpadY, 11.0f * scale, rgba(0.85f, 0.93f, 1.0f, hudOp * 0.90f));
        }
    }

    // -------------------------------------------------------------
    // Realtime Hardware Monitor HUD (FPS, Frame Time, CPU, GPU, RAM)
    // -------------------------------------------------------------
    if (mSettings.perfHudMode > 0 && !mEditLayoutMode) {
        char hudBuf[144];
        if (mSettings.perfHudMode == 1) {
            snprintf(hudBuf, sizeof(hudBuf), "%.1f FPS  •  %.1f ms", mCurrentFps, mAvgFrameTimeMs);
        } else {
            snprintf(hudBuf, sizeof(hudBuf), "%.1f FPS  •  %.1f ms  •  CPU %.0f%%  •  GPU %.0f%%  •  RAM %.0f MB",
                     mCurrentFps, mAvgFrameTimeMs, mCpuUsagePercent, mGpuUsagePercent, mRamUsageMB);
        }

        float hudTextScale = 0.28f * uiScale;
        float textW = measureText(hudBuf, hudTextScale);
        float hudPadX = 14.0f * uiScale;
        float hudPadY = 6.0f * uiScale;
        float hudW = textW + hudPadX * 2.0f;
        float hudH = 26.0f * uiScale;
        float hudX = w * 0.5f - hudW * 0.5f; // Perfectly centered at top of screen
        float hudY = 14.0f * uiScale;

        // Frosted dark pill container
        drawRoundRect(hudX, hudY, hudX + hudW, hudY + hudH, 13.0f * uiScale, rgba(0.06f, 0.08f, 0.12f, 0.85f));
        drawRoundRect(hudX + 1.0f, hudY + 1.0f, hudX + hudW - 1.0f, hudY + hudH - 1.0f, 12.0f * uiScale, rgba(0.24f, 0.32f, 0.44f, 0.55f));

        // Pulsing status LED
        Color ledColor = (mCurrentFps >= 55.0f) ? rgba(0.30f, 0.90f, 0.50f, 1.0f) :
                         (mCurrentFps >= 30.0f) ? rgba(1.0f, 0.75f, 0.20f, 1.0f) :
                                                  rgba(0.95f, 0.30f, 0.25f, 1.0f);
        drawCircle(hudX + 10.0f * uiScale, hudY + hudH * 0.5f, 3.5f * uiScale, ledColor);

        // Render stats typography
        drawText(hudBuf, hudX + 18.0f * uiScale, hudY + hudH * 0.5f + 4.0f * uiScale, hudTextScale, rgba(0.92f, 0.95f, 1.0f, 0.95f), 0);
    }

    // -------------------------------------------------------------
    // Material 3 Settings Tab Dock (Pinned to Top-Left Edge)
    // -------------------------------------------------------------
    if (mDrawerAnim < 0.95f) {
        float tabW = 56.0f * uiScale;
        float tabH = 48.0f * uiScale;
        float tabY = 22.0f * uiScale;
        float tabPr = mBtnCfgPressed ? 0.92f : 1.0f;

        // Docked pill container: rounded right edge
        drawRoundRect(-10.0f, tabY, tabW * tabPr, tabY + tabH, 24.0f * uiScale, rgba(0.149f, 0.157f, 0.180f, 0.94f));
        drawRoundRect(-10.0f, tabY, tabW * tabPr, tabY + tabH, 24.0f * uiScale, rgba(0.267f, 0.278f, 0.310f, 0.60f));

        // Rotating Material 3 Cogwheel icon in Google Accent Blue
        drawCogwheel(tabW * 0.50f, tabY + tabH * 0.5f, 15.0f * uiScale, rgba(0.659f, 0.780f, 0.980f, 1.0f), cogRot);

        // Direct Quick Save Pill Button
        float qsX = tabW + 16.0f * uiScale;
        float qsW = 58.0f * uiScale;
        drawRoundRect(qsX, tabY + 4.0f * uiScale, qsX + qsW, tabY + tabH - 4.0f * uiScale, 16.0f * uiScale, rgba(0.12f, 0.14f, 0.18f, 0.88f));
        drawRoundRect(qsX, tabY + 4.0f * uiScale, qsX + qsW, tabY + tabH - 4.0f * uiScale, 16.0f * uiScale, rgba(0.25f, 0.45f, 0.78f, 0.65f));
        drawText("SAVE", qsX + qsW * 0.5f, tabY + tabH * 0.5f + 4.0f * uiScale, 0.28f * uiScale, rgba(0.92f, 0.96f, 1.0f, 0.95f), 1);
    }

    // -------------------------------------------------------------
    // Draw Edit Layout Overlay
    // -------------------------------------------------------------
    if (mEditLayoutMode) {
        // Scrim
        drawRoundRect(0, 0, w, h, 0.0f, rgba(0.0f, 0.0f, 0.0f, 0.45f));

        // M3 Top bar card
        drawRoundRect(w * 0.5f - 240.0f, 30.0f, w * 0.5f + 240.0f, 95.0f, 20.0f, rgba(0.12f, 0.14f, 0.18f, 0.96f));
        drawText("DRAG ANY CONTROL TO MOVE IT", w * 0.5f, 55.0f, 0.46f, rgba(0.90f, 0.92f, 0.96f, 1.0f), 1);

        // Save button (M3 Primary Pill)
        float saveBtnX = w * 0.5f - 110.0f;
        drawRoundRect(saveBtnX, 65.0f, saveBtnX + 100.0f, 90.0f, 12.0f, rgba(0.043f, 0.341f, 0.816f, 0.95f));
        drawText("SAVE", saveBtnX + 50.0f, 83.0f, 0.42f, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

        // Reset button (M3 Outline Pill)
        float rstBtnX = w * 0.5f + 10.0f;
        drawRoundRect(rstBtnX, 65.0f, rstBtnX + 100.0f, 90.0f, 12.0f, rgba(0.35f, 0.18f, 0.20f, 0.95f));
        drawText("RESET", rstBtnX + 50.0f, 83.0f, 0.42f, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

        // Only currently visible controls are draggable in this editor.
        if (mSettings.showStick) {
            float defaultStickX = 175.0f * scale + mSettings.stickOffsetX;
            float defaultStickY = h - 175.0f * scale + mSettings.stickOffsetY;
            drawRing(defaultStickX, defaultStickY, 120.0f * scale, 0.96f, rgba(0.40f, 0.65f, 0.98f, 0.85f));
            drawText("JOYSTICK", defaultStickX, defaultStickY - 130.0f * scale, 0.45f, rgba(0.40f, 0.65f, 0.98f, 0.95f), 1);
        }

        // Button cluster bounding outline
        float btnBaseX = w - 160.0f * scale + mSettings.buttonOffsetX;
        float btnBaseY = h - 160.0f * scale + mSettings.buttonOffsetY;
        if (mSettings.showButtonA || mSettings.showButtonB || mSettings.showButtonZ ||
            mSettings.showButtonSpin || mSettings.showPauseButton) {
            drawRing(btnBaseX - 60.0f * scale, btnBaseY - 60.0f * scale, 160.0f * scale, 0.96f, rgba(0.40f, 0.65f, 0.98f, 0.85f));
        }

        // Each action button gets its own grip ring so it can be dragged apart
        // from the others (e.g. Z away from A).
        ButtonLayout editBl = buttonLayout(screenW, screenH);
        struct EditBtn { float cx, cy, r; const char* label; bool visible; } editBtns[5] = {
            {editBl.ax, editBl.ay, editBl.ar, "A", mSettings.showButtonA},
            {editBl.bx, editBl.by, editBl.br, "B", mSettings.showButtonB},
            {editBl.zx, editBl.zy, editBl.zr, "Z", mSettings.showButtonZ},
            {editBl.spinX, editBl.spinY, editBl.spinR, "SPIN", mSettings.showButtonSpin},
            {editBl.pauseX, editBl.pauseY, editBl.pauseR, "PAUSE", mSettings.showPauseButton},
        };
        for (const EditBtn& b : editBtns) {
            if (!b.visible) continue;
            drawCircle(b.cx, b.cy, b.r, rgba(0.06f, 0.08f, 0.13f, 0.55f));
            drawRing(b.cx, b.cy, b.r, 0.92f, rgba(0.40f, 0.65f, 0.98f, 0.95f));
            drawText(b.label, b.cx, b.cy + 8.0f * scale, 0.42f * scale, rgba(0.92f, 0.95f, 1.0f, 0.95f), 1);
        }
        drawText("BUTTONS", btnBaseX - 20.0f * scale, btnBaseY - 240.0f * scale, 0.45f, rgba(0.40f, 0.65f, 0.98f, 0.95f), 1);

        // Camera D-Pad bounding outline
        if (mSettings.showDpad) {
            float baseDpadX = mSettings.dpadOnRight ? (w - 380.0f * scale) : (140.0f * scale);
            float baseDpadY = mSettings.dpadOnRight ? (h - 220.0f * scale) : (h - 390.0f * scale);
            float curDpadX = baseDpadX + mSettings.dpadOffsetX;
            float curDpadY = baseDpadY + mSettings.dpadOffsetY;
            drawRing(curDpadX, curDpadY, 78.0f * scale, 0.96f, rgba(0.40f, 0.65f, 0.98f, 0.85f));
            drawText("CAMERA D-PAD", curDpadX, curDpadY - 88.0f * scale, 0.42f, rgba(0.40f, 0.65f, 0.98f, 0.95f), 1);
        }
    }

    // -------------------------------------------------------------
    // Material 3 Side Sheet Navigation Drawer (Tabbed & Scrollable)
    // -------------------------------------------------------------
    if (mDrawerAnim > 0.001f) {
        // Scrim backdrop (smoothly fades with animation)
        drawRoundRect(0, 0, w, h, 0.0f, rgba(0.0f, 0.0f, 0.0f, mDrawerAnim * 0.52f));

        // Side Sheet container: Surface Container (#1B1D23) with 28dp rounded right corners
        drawRoundRect(drawerX - 60.0f, 0.0f, drawerX + drawerW, h, 28.0f * uiScale, rgba(0.106f, 0.114f, 0.137f, 0.98f));
        // Right edge outline border
        drawRoundRect(drawerX + drawerW - 2.0f, 0.0f, drawerX + drawerW, h, 0.0f, rgba(0.267f, 0.278f, 0.310f, 0.55f));

        float padX = 22.0f * uiScale;
        float contentW = drawerW - padX * 2.0f;
        float contentTop = 112.0f * uiScale;
        float contentBottom = h - 56.0f * uiScale;
        const bool showingGeneralTab = mSettingsTab == 0;
        float totalContentH = showingGeneralTab ? (1540.0f * uiScale) :
                              (mSettingsTab == 4 ? (560.0f * uiScale) :
                               (mSettingsTab == 3 ? (950.0f * uiScale) :
                               ((mPlusEdition && mSettingsTab == 1) ? (760.0f * uiScale) : (580.0f * uiScale))));
        float maxScroll = std::max(0.0f, totalContentH - (contentBottom - contentTop));

        // Smooth scroll damping
        if (fabsf(mDrawerScrollY - mDrawerScrollTarget) > 0.5f) {
            mDrawerScrollY += (mDrawerScrollTarget - mDrawerScrollY) * (1.0f - expf(-18.0f * dt));
            mDirty = true;
        } else {
            mDrawerScrollY = mDrawerScrollTarget;
        }
        mDrawerScrollY = std::max(0.0f, std::min(mDrawerScrollY, maxScroll));
        mDrawerScrollTarget = std::max(0.0f, std::min(mDrawerScrollTarget, maxScroll));

        float swW = 50.0f * uiScale, swH = 28.0f * uiScale;

        // ---------------------------------------------------------
        // A. Scrollable Content Layer
        // ---------------------------------------------------------
        if (mSettingsTab == 0) {
            // General Tab Settings Items
            auto renderRow = [&](float relY, float rowH, auto func) {
                float drawY = contentTop + relY - mDrawerScrollY;
                if (drawY + rowH >= contentTop - 12.0f && drawY <= contentBottom + 12.0f) {
                    func(drawY);
                }
            };

            // Row 0: Quick Save & Quick Load Anywhere Action Buttons
            renderRow(8.0f * uiScale, 48.0f * uiScale, [&](float y) {
                float bW = (contentW - 10.0f * uiScale) * 0.5f;
                float btnY = y + 4.0f * uiScale;

                // Quick Save (Green)
                float sX0 = drawerX + padX;
                float sX1 = sX0 + bW;
                drawRoundRect(sX0, btnY, sX1, btnY + 40.0f * uiScale, 14.0f * uiScale, rgba(0.06f, 0.42f, 0.32f, 0.95f));
                drawRoundRect(sX0, btnY, sX1, btnY + 40.0f * uiScale, 14.0f * uiScale, rgba(0.25f, 0.85f, 0.65f, 0.60f));
                drawCircle(sX0 + 18.0f * uiScale, btnY + 20.0f * uiScale, 5.0f * uiScale, rgba(0.35f, 0.98f, 0.75f, 1.0f));
                drawText("QUICK SAVE", (sX0 + sX1) * 0.5f + 6.0f * uiScale, btnY + 25.0f * uiScale, 0.35f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

                // Quick Load (Blue)
                float lX0 = sX1 + 10.0f * uiScale;
                float lX1 = lX0 + bW;
                drawRoundRect(lX0, btnY, lX1, btnY + 40.0f * uiScale, 14.0f * uiScale, rgba(0.12f, 0.28f, 0.55f, 0.95f));
                drawRoundRect(lX0, btnY, lX1, btnY + 40.0f * uiScale, 14.0f * uiScale, rgba(0.35f, 0.65f, 0.98f, 0.60f));
                drawCircle(lX0 + 18.0f * uiScale, btnY + 20.0f * uiScale, 5.0f * uiScale, rgba(0.45f, 0.80f, 1.0f, 1.0f));
                drawText("QUICK LOAD", (lX0 + lX1) * 0.5f + 6.0f * uiScale, btnY + 25.0f * uiScale, 0.35f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);
            });

            // Row 1: Visibility Switch
            renderRow(62.0f * uiScale, 46.0f * uiScale, [&](float y) {
                drawText("On-Screen Controls", drawerX + padX, y + 20.0f * uiScale, 0.40f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Show HUD buttons", drawerX + padX, y + 36.0f * uiScale, 0.29f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.showControls);
            });

            // Row 2: Camera D-Pad Switch
            renderRow(112.0f * uiScale, 46.0f * uiScale, [&](float y) {
                drawText("Camera D-Pad", drawerX + padX, y + 20.0f * uiScale, 0.40f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Limited camera rotation & reset", drawerX + padX, y + 36.0f * uiScale, 0.29f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.showDpad);
            });

            // Row 3: D-Pad Position Segmented Button
            renderRow(162.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("D-Pad Position", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* dpadSideOpts[2] = {"Left Side", "Right Side"};
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, dpadSideOpts, 2, mSettings.dpadOnRight ? 1 : 0);
            });

            // Row 4: Aspect-safe image sizing. The activity remains immersive;
            // this selector only scales the game image inside the available area.
            renderRow(220.0f * uiScale, 46.0f * uiScale, [&](float y) {
                drawText(mSettings.fullscreen ? "Widescreen Hor+ (full screen)" : "16:9 fit (side bands)",
                         drawerX + padX, y + 15.0f * uiScale, 0.36f * uiScale,
                         rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawM3Switch(drawerX + drawerW - padX - 44.0f * uiScale,
                             y + 2.0f * uiScale, 44.0f * uiScale, 20.0f * uiScale,
                             mSettings.fullscreen);
                const char* sizeOpts[3] = {"85%", "95%", "100%"};
                const int sizeIdx = std::max(0, std::min(2, mSettings.screenScaleIndex));
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale,
                                   contentW, 18.0f * uiScale, sizeOpts, 3, sizeIdx);
            });

            // Row 5: Full Touch Mode Switch
            renderRow(270.0f * uiScale, 46.0f * uiScale, [&](float y) {
                drawText("Full Touch Mode", drawerX + padX, y + 20.0f * uiScale, 0.40f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Tap to jump/advance, flick to spin", drawerX + padX, y + 36.0f * uiScale, 0.29f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.fullTouchMode);
            });

            // Row 6: Joystick Mode Segmented Button
            renderRow(320.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Joystick Mode", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* joyOpts[2] = {"Dynamic", "Fixed"};
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, joyOpts, 2, mSettings.dynamicStick ? 0 : 1);
            });

            // Row 7: Controls Opacity Segmented Button
            renderRow(378.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Controls Opacity", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* opOpts[4] = {"25%", "50%", "75%", "100%"};
                int opIdx = (fabsf(mSettings.opacity - 0.25f) < 0.12f) ? 0 : (fabsf(mSettings.opacity - 0.50f) < 0.12f) ? 1 : (fabsf(mSettings.opacity - 0.75f) < 0.12f) ? 2 : 3;
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, opOpts, 4, opIdx);
            });

            // Row 8: Button Size Segmented Button
            renderRow(436.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Button Size", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* scOpts[3] = {"Small", "Medium", "Large"};
                int scIdx = (fabsf(mSettings.scale - 0.85f) < 0.08f) ? 0 : (fabsf(mSettings.scale - 1.25f) < 0.08f) ? 2 : 1;
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, scOpts, 3, scIdx);
            });

            // Row 9: Layout Reposition & Reset Buttons
            renderRow(494.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Controls Layout", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                float btnY = y + 22.0f * uiScale;
                float b1X0 = drawerX + padX;
                float b1X1 = drawerX + padX + contentW * 0.56f - 4.0f * uiScale;
                float b2X0 = drawerX + padX + contentW * 0.56f + 4.0f * uiScale;
                float b2X1 = drawerX + drawerW - padX;
                drawRoundRect(b1X0, btnY, b1X1, btnY + 34.0f * uiScale, 14.0f * uiScale, rgba(0.18f, 0.23f, 0.31f, 0.95f));
                drawText("Reposition", (b1X0 + b1X1) * 0.5f, btnY + 22.0f * uiScale, 0.36f * uiScale, rgba(0.659f, 0.780f, 0.980f, 1.0f), 1);
                drawRoundRect(b2X0, btnY, b2X1, btnY + 34.0f * uiScale, 14.0f * uiScale, rgba(0.35f, 0.15f, 0.17f, 0.95f));
                drawText("Reset", (b2X0 + b2X1) * 0.5f, btnY + 22.0f * uiScale, 0.36f * uiScale, rgba(1.0f, 0.72f, 0.72f, 1.0f), 1);
            });

            // Row 10: Diagnostics and system logs button
            renderRow(552.0f * uiScale, 48.0f * uiScale, [&](float y) {
                float btnY = y + 8.0f * uiScale;
                float bX0 = drawerX + padX;
                float bX1 = drawerX + drawerW - padX;
                drawRoundRect(bX0, btnY, bX1, btnY + 36.0f * uiScale, 14.0f * uiScale, rgba(0.12f, 0.28f, 0.44f, 0.95f));
                drawRoundRect(bX0, btnY, bX1, btnY + 36.0f * uiScale, 14.0f * uiScale, rgba(0.38f, 0.74f, 0.98f, 0.50f));
                drawText("Registro y errores", (bX0 + bX1) * 0.5f, btnY + 23.0f * uiScale, 0.38f * uiScale, rgba(0.85f, 0.94f, 1.0f, 1.0f), 1);
            });

            // Row 11: Hardware Monitor HUD Segmented Button
            renderRow(606.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Hardware Monitor HUD", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* hudOpts[3] = {"Off", "FPS", "Full Stats"};
                int hudIdx = std::max(0, std::min(2, mSettings.perfHudMode));
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, hudOpts, 3, hudIdx);
            });

            // Row 12: Target Frame Rate Segmented Button
            renderRow(664.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Target Frame Rate", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* fpsOpts[5] = {"30", "60", "90", "120", "144"};
                int fpsIdx = (mSettings.targetFps >= 144) ? 4 : (mSettings.targetFps >= 120) ? 3 : (mSettings.targetFps >= 90) ? 2 : (mSettings.targetFps >= 60) ? 1 : 0;
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, fpsOpts, 5, fpsIdx);
            });

            // Row 13: Graphics Backend Segmented Button
            renderRow(722.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Graphics Backend", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* backendOpts[2] = {"OpenGL ES 3.2", "Vulkan 1.0"};
                int bkIdx = (mSettings.graphicsBackend == 1) ? 1 : 0;
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, backendOpts, 2, bkIdx);
            });

            // Row 14: Game Speed Multiplier (Fast-Forward / Turbo)
            renderRow(776.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Game / Dialogue Speed", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* spdOpts[5] = {"1.0x", "1.5x", "2.0x", "3.0x", "4.0x"};
                int spdIdx = std::max(0, std::min(4, mSettings.gameSpeed - 1));
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, spdOpts, 5, spdIdx);
            });

            // Row 15: Game Language Selector
            renderRow(834.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Game Language", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* langOpts[6] = {"Auto", "EN", "ES", "FR", "DE", "IT"};
                int langIdx = std::max(0, std::min(5, mSettings.languageChoice));
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, langOpts, 6, langIdx);
            });

            // Row 16: Render Resolution Segmented Button
            renderRow(892.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Render Resolution", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* resOpts[5] = {"480p", "720p", "1080p", "1440p", "4K"};
                int resIdx = std::max(0, std::min(4, mSettings.renderScale));
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, resOpts, 5, resIdx);
            });

            // Row 16b: Display Refresh & Framerate Pacing
            renderRow(950.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Display Refresh & Target FPS", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* fpsOpts[4] = {"60 Hz", "90 Hz", "120 Hz", "144 Hz"};
                int fpsIdx = (mSettings.targetFps >= 144) ? 3 : (mSettings.targetFps >= 120) ? 2 : (mSettings.targetFps >= 90) ? 1 : 0;
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, fpsOpts, 4, fpsIdx);
            });

            // Row 17: Vibration Haptics Switch
            renderRow(1008.0f * uiScale, 46.0f * uiScale, [&](float y) {
                drawText("Vibration Haptics", drawerX + padX, y + 20.0f * uiScale, 0.40f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Tactile feedback on button presses", drawerX + padX, y + 36.0f * uiScale, 0.29f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.haptics);
            });

            // Row 18: Battery Saver Switch
            renderRow(1062.0f * uiScale, 48.0f * uiScale, [&](float y) {
                drawText("Battery Saver / Cool Mode", drawerX + padX, y + 18.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("30 FPS + 720p cap on GLES and Vulkan", drawerX + padX, y + 34.0f * uiScale, 0.28f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.batterySaver);
            });

            // Row 19: Gyroscope Star Pointer Switch
            renderRow(1116.0f * uiScale, 48.0f * uiScale, [&](float y) {
                drawText("Gyro Star Pointer Aiming", drawerX + padX, y + 18.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Tilt phone to steer Star Pointer", drawerX + padX, y + 34.0f * uiScale, 0.28f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.gyroAiming);
            });

            // Row 19b: Gyro Invert Axes
            renderRow(1170.0f * uiScale, 46.0f * uiScale, [&](float y) {
                float b1X0 = drawerX + padX;
                float b1X1 = drawerX + padX + contentW * 0.5f - 4.0f * uiScale;
                float b2X0 = drawerX + padX + contentW * 0.5f + 4.0f * uiScale;
                float b2X1 = drawerX + drawerW - padX;

                Color c1 = mSettings.gyroInvertX ? rgba(0.043f, 0.341f, 0.816f, 0.95f) : rgba(0.18f, 0.20f, 0.25f, 0.85f);
                Color c2 = mSettings.gyroInvertY ? rgba(0.043f, 0.341f, 0.816f, 0.95f) : rgba(0.18f, 0.20f, 0.25f, 0.85f);
                drawRoundRect(b1X0, y + 8.0f * uiScale, b1X1, y + 38.0f * uiScale, 10.0f * uiScale, c1);
                drawText(mSettings.gyroInvertX ? "Invert X: ON" : "Invert X: OFF", (b1X0 + b1X1) * 0.5f, y + 25.0f * uiScale, 0.32f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

                drawRoundRect(b2X0, y + 8.0f * uiScale, b2X1, y + 38.0f * uiScale, 10.0f * uiScale, c2);
                drawText(mSettings.gyroInvertY ? "Invert Y: ON" : "Invert Y: OFF", (b2X0 + b2X1) * 0.5f, y + 25.0f * uiScale, 0.32f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);
            });

            // Row 19c: Relative Pointer Switch (Issue #11)
            renderRow(1222.0f * uiScale, 48.0f * uiScale, [&](float y) {
                drawText("Relative Star Pointer (Trackpad)", drawerX + padX, y + 18.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Drag anywhere to steer pointer without blocking view", drawerX + padX, y + 34.0f * uiScale, 0.28f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.relativePointer);
            });

            // Row 19d: Screen Orientation Segmented Button (Issues #18 & #19)
            renderRow(1276.0f * uiScale, 56.0f * uiScale, [&](float y) {
                drawText("Screen Orientation", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* orientOpts[3] = {"Auto Sensor", "Landscape", "Reverse"};
                drawSegmentedGroup(drawerX + padX, y + 22.0f * uiScale, contentW, 34.0f * uiScale, orientOpts, 3, mSettings.screenOrientationChoice);
            });

            // Row 20: Save Data Backup (Downloads)
            renderRow(1338.0f * uiScale, 56.0f * uiScale, [&](float y) {
                drawText("Save Data Backup (Downloads)", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                float btnY = y + 22.0f * uiScale;
                float b1X0 = drawerX + padX;
                float b1X1 = drawerX + padX + contentW * 0.5f - 4.0f * uiScale;
                float b2X0 = drawerX + padX + contentW * 0.5f + 4.0f * uiScale;
                float b2X1 = drawerX + drawerW - padX;
                drawRoundRect(b1X0, btnY, b1X1, btnY + 32.0f * uiScale, 12.0f * uiScale, rgba(0.12f, 0.28f, 0.44f, 0.95f));
                drawText("Export Save", (b1X0 + b1X1) * 0.5f, btnY + 21.0f * uiScale, 0.34f * uiScale, rgba(0.85f, 0.94f, 1.0f, 1.0f), 1);
                drawRoundRect(b2X0, btnY, b2X1, btnY + 32.0f * uiScale, 12.0f * uiScale, rgba(0.18f, 0.34f, 0.24f, 0.95f));
                drawText("Import Save", (b2X0 + b2X1) * 0.5f, btnY + 21.0f * uiScale, 0.34f * uiScale, rgba(0.85f, 1.0f, 0.90f, 1.0f), 1);
            });

            // Row 21: Realtime Hardware & Device Info Card
            renderRow(1400.0f * uiScale, 64.0f * uiScale, [&](float y) {
                float cardX0 = drawerX + padX;
                float cardX1 = drawerX + drawerW - padX;
                float cardH = 60.0f * uiScale;
                drawRoundRect(cardX0, y, cardX1, y + cardH, 12.0f * uiScale, rgba(0.10f, 0.12f, 0.16f, 0.90f));
                drawRoundRect(cardX0 + 1.0f, y + 1.0f, cardX1 - 1.0f, y + cardH - 1.0f, 11.0f * uiScale, rgba(0.20f, 0.25f, 0.34f, 0.50f));

                char gpuLine[128], resLine[128], apiLine[128];
                snprintf(gpuLine, sizeof(gpuLine), "GPU: %s", mGpuRendererName.empty() ? "OpenGL ES 3.2" : mGpuRendererName.c_str());
                const char* resStr = (mSettings.renderScale == 0) ? "480p" :
                                     (mSettings.renderScale == 1) ? "720p" :
                                     (mSettings.renderScale == 2) ? "1080p" :
                                     (mSettings.renderScale == 3) ? "1440p" : "4K";
                snprintf(resLine, sizeof(resLine), "Display: %dx%d (%s target • %s)",
                         (int)w, (int)h, resStr,
                         mSettings.fullscreen ? "Hor+" : "16:9 fit");
                snprintf(apiLine, sizeof(apiLine), "Active Engine: %s • Native ARM64", (mActiveBackend == 1) ? "Vulkan 1.0" : "OpenGL ES 3.2 (fallback)");

                drawText(gpuLine, cardX0 + 12.0f * uiScale, y + 18.0f * uiScale, 0.28f * uiScale, rgba(0.40f, 0.75f, 1.0f, 0.95f), 0);
                drawText(resLine, cardX0 + 12.0f * uiScale, y + 34.0f * uiScale, 0.26f * uiScale, rgba(0.80f, 0.84f, 0.90f, 0.85f), 0);
                drawText(apiLine, cardX0 + 12.0f * uiScale, y + 49.0f * uiScale, 0.24f * uiScale, rgba(0.55f, 0.60f, 0.68f, 0.80f), 0);
            });

            // Row 22: Return to Setup / Launcher Button
            renderRow(1472.0f * uiScale, 52.0f * uiScale, [&](float y) {
                float bX0 = drawerX + padX;
                float bX1 = drawerX + drawerW - padX;
                float btnY = y + 4.0f * uiScale;
                drawRoundRect(bX0, btnY, bX1, btnY + 40.0f * uiScale, 14.0f * uiScale, rgba(0.40f, 0.12f, 0.15f, 0.95f));
                drawRoundRect(bX0, btnY, bX1, btnY + 40.0f * uiScale, 14.0f * uiScale, rgba(0.90f, 0.25f, 0.30f, 0.60f));
                drawText("Exit to Setup Launcher", (bX0 + bX1) * 0.5f, btnY + 25.0f * uiScale, 0.38f * uiScale, rgba(1.0f, 0.88f, 0.88f, 1.0f), 1);
            });
        }
        else if (mPlusEdition && mSettingsTab == 1) {
            // Mod Manager Tab Content
            auto renderRow = [&](float relY, float rowH, auto func) {
                float drawY = contentTop + relY - mDrawerScrollY;
                if (drawY + rowH >= contentTop - 12.0f && drawY <= contentBottom + 12.0f) {
                    func(drawY);
                }
            };

            // Section 1: NVIDIA SHIELD REMASTER PACK
            renderRow(4.0f * uiScale, 36.0f * uiScale, [&](float y) {
                drawText("NVIDIA Shield Remaster Pack", drawerX + padX, y + 16.0f * uiScale, 0.38f * uiScale, rgba(0.38f, 0.78f, 0.98f, 1.0f), 0);
                drawText("Authentic 1080p AI assets from Shield TV port", drawerX + padX, y + 32.0f * uiScale, 0.27f * uiScale, rgba(0.68f, 0.75f, 0.88f, 0.85f), 0);
            });

            // Mod 1: NVIDIA Shield HD Textures
            renderRow(44.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Shield HD Textures", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("1080p AI Remastered UI, characters & HUD", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modShieldTextures);
            });

            // Mod 2: NVIDIA Shield 60FPS Cutscenes
            renderRow(96.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Shield 60FPS Cutscenes", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("H.264 MP4 hardware video stream", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modShieldCutscenes);
            });

            // Mod 3: Shield Controller Curves
            renderRow(148.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Calibrated Analog Curves", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("smg_controller.ini ray surfing & pointer damping", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modShieldControls);
            });

            // Mod 4: Classic Gamepad ABXY Prompts
            renderRow(200.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Modern Gamepad ABXY UI", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Replaces Wii Remote icons with Xbox/Shield buttons", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modClassicGamepadUI);
            });

            // Section 2: GAMEPLAY & CHEAT MODS
            renderRow(254.0f * uiScale, 36.0f * uiScale, [&](float y) {
                drawText("Gameplay & Cheats", drawerX + padX, y + 16.0f * uiScale, 0.38f * uiScale, rgba(0.40f, 0.85f, 0.65f, 1.0f), 0);
                drawText("Developer sandbox & flight utilities", drawerX + padX, y + 32.0f * uiScale, 0.27f * uiScale, rgba(0.68f, 0.75f, 0.88f, 0.85f), 0);
            });

            // Mod 5: Fly Mode Switch
            renderRow(292.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Fly Mode", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Hold A to ascend, hold Z to descend", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modFly);
            });

            // Mod 6: Cutscene Prevention
            renderRow(344.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Cutscene Prevention", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Instantly skip dialogues & cinematic scenes", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modSkipCutscenes);
            });

            // Mod 7: Infinite Health
            renderRow(396.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Infinite Health", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Invincible; escape black holes automatically", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modInfiniteHealth);
            });

            // Mod 8: Infinite Lives (99)
            renderRow(448.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Infinite Lives (99)", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Lock Mario's life counter to 99 lives", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modInfiniteLives);
            });

            // Mod 9: Infinite Star Bits (999)
            renderRow(500.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Infinite Star Bits (999)", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Max out Star Bits pouch to 999", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modInfiniteStarBits);
            });

            // Mod 10: Turbo Game Speed
            renderRow(552.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("Turbo Game Speed", drawerX + padX, y + 14.0f * uiScale, 0.38f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* spdOpts[5] = {"1.0x", "1.5x", "2.0x", "3.0x", "4.0x"};
                drawSegmentedGroup(drawerX + padX, y + 20.0f * uiScale, contentW, 32.0f * uiScale, spdOpts, 5, mSettings.gameSpeed - 1);
            });

            // Section 3: EXTERNAL RIIVOLUTION MODS
            renderRow(610.0f * uiScale, 36.0f * uiScale, [&](float y) {
                drawText("Riivolution Modding System", drawerX + padX, y + 16.0f * uiScale, 0.38f * uiScale, rgba(0.95f, 0.75f, 0.35f, 1.0f), 0);
                drawText("Load custom XML mods from /sdcard/GalaxyData/mods", drawerX + padX, y + 32.0f * uiScale, 0.27f * uiScale, rgba(0.68f, 0.75f, 0.88f, 0.85f), 0);
            });

            // Mod 11: Riivolution Switch
            renderRow(648.0f * uiScale, 50.0f * uiScale, [&](float y) {
                drawText("Enable Riivolution Mods", drawerX + padX, y + 18.0f * uiScale, 0.39f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                drawText("Patch game files on the fly from SD card", drawerX + padX, y + 34.0f * uiScale, 0.27f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.85f), 0);
                drawM3Switch(drawerX + drawerW - padX - swW, y + 10.0f * uiScale, swW, swH, mSettings.modRiivolution);
            });

            // Mod 12: Scan & Reload Mods Button
            renderRow(700.0f * uiScale, 48.0f * uiScale, [&](float y) {
                float bX0 = drawerX + padX;
                float bX1 = drawerX + drawerW - padX;
                drawRoundRect(bX0, y + 4.0f * uiScale, bX1, y + 42.0f * uiScale, 14.0f * uiScale, rgba(0.18f, 0.22f, 0.30f, 0.95f));
                drawRoundRect(bX0, y + 4.0f * uiScale, bX1, y + 42.0f * uiScale, 14.0f * uiScale, rgba(0.35f, 0.50f, 0.75f, 0.65f));
                drawText("SCAN & RELOAD MOD DIRECTORY", (bX0 + bX1) * 0.5f, y + 26.0f * uiScale, 0.34f * uiScale, rgba(0.85f, 0.92f, 1.0f, 1.0f), 1);
            });
        }
        else if (mSettingsTab == 3) {
            // Touch and physical-controller settings are independent from the
            // Plus-only mod page, so every edition can configure input.
            auto renderRow = [&](float relY, float rowH, auto func) {
                float drawY = contentTop + relY - mDrawerScrollY;
                if (drawY + rowH >= contentTop - 12.0f && drawY <= contentBottom + 12.0f) {
                    func(drawY);
                }
            };
            auto drawToggleCard = [&](float x0, float x1, float y, const char* label, bool enabled) {
                drawRoundRect(x0, y + 2.0f * uiScale, x1, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.13f, 0.15f, 0.19f, 0.96f));
                drawRoundRect(x0, y + 2.0f * uiScale, x1, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.31f, 0.34f, 0.40f, 0.52f));
                drawText(label, x0 + 10.0f * uiScale, y + 21.0f * uiScale,
                         0.29f * uiScale, rgba(0.91f, 0.93f, 0.97f, 1.0f), 0);
                drawText(enabled ? "VISIBLE" : "OCULTO", x0 + 10.0f * uiScale,
                         y + 38.0f * uiScale, 0.23f * uiScale,
                         enabled ? rgba(0.48f, 0.88f, 0.73f, 0.95f) : rgba(0.62f, 0.65f, 0.71f, 0.88f), 0);
                drawM3Switch(x1 - swW - 8.0f * uiScale, y + 13.0f * uiScale, swW, swH, enabled);
            };
            auto drawBindCard = [&](float x0, float x1, float y, int action) {
                const int* binding = controllerBinding(mSettings, action);
                const bool listening = mControllerCaptureAction == action;
                const std::string keyName = listening ? "PULSA MANDO" : controllerKeyName(binding ? *binding : -1);
                drawRoundRect(x0, y + 2.0f * uiScale, x1, y + 48.0f * uiScale,
                              12.0f * uiScale, listening ? rgba(0.10f, 0.32f, 0.30f, 0.98f) : rgba(0.13f, 0.15f, 0.19f, 0.96f));
                drawRoundRect(x0, y + 2.0f * uiScale, x1, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.31f, 0.34f, 0.40f, 0.52f));
                drawText(controllerActionName(action), x0 + 10.0f * uiScale, y + 20.0f * uiScale,
                         0.29f * uiScale, rgba(0.91f, 0.93f, 0.97f, 1.0f), 0);
                drawText(keyName.c_str(), x0 + 10.0f * uiScale, y + 38.0f * uiScale,
                         0.24f * uiScale, listening ? rgba(0.49f, 0.91f, 0.77f, 1.0f) : rgba(0.70f, 0.75f, 0.84f, 0.95f), 0);
            };

            const float leftX = drawerX + padX;
            const float rightX = leftX + contentW * 0.5f + 4.0f * uiScale;
            const float halfW = (contentW - 8.0f * uiScale) * 0.5f;
            renderRow(4.0f * uiScale, 36.0f * uiScale, [&](float y) {
                drawText("CONTROLES TACTILES", leftX, y + 17.0f * uiScale,
                         0.38f * uiScale, rgba(0.42f, 0.88f, 0.76f, 1.0f), 0);
                drawText("Mover, ajustar o volver a mostrar botones", leftX, y + 33.0f * uiScale,
                         0.25f * uiScale, rgba(0.70f, 0.75f, 0.84f, 0.88f), 0);
            });
            renderRow(44.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawToggleCard(leftX, leftX + contentW, y, "Capa tactil completa", mSettings.showControls);
            });
            renderRow(98.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawToggleCard(leftX, leftX + halfW, y, "Boton A", mSettings.showButtonA);
                drawToggleCard(rightX, rightX + halfW, y, "Boton B", mSettings.showButtonB);
            });
            renderRow(152.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawToggleCard(leftX, leftX + halfW, y, "Boton Z", mSettings.showButtonZ);
                drawToggleCard(rightX, rightX + halfW, y, "Boton SPIN", mSettings.showButtonSpin);
            });
            renderRow(206.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawToggleCard(leftX, leftX + halfW, y, "Joystick tactil", mSettings.showStick);
                drawToggleCard(rightX, rightX + halfW, y, "Control de camara", mSettings.showDpad);
            });
            renderRow(260.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawToggleCard(leftX, leftX + contentW, y, "Boton pausa (+)", mSettings.showPauseButton);
            });
            renderRow(314.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("TAMANO", leftX, y + 14.0f * uiScale, 0.32f * uiScale, rgba(0.90f, 0.92f, 0.96f, 1.0f), 0);
                const char* sizeOpts[3] = {"85%", "100%", "125%"};
                const int idx = fabsf(mSettings.scale - 0.85f) < 0.08f ? 0 : fabsf(mSettings.scale - 1.25f) < 0.08f ? 2 : 1;
                drawSegmentedGroup(leftX, y + 20.0f * uiScale, contentW, 32.0f * uiScale, sizeOpts, 3, idx);
            });
            renderRow(370.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("OPACIDAD", leftX, y + 14.0f * uiScale, 0.32f * uiScale, rgba(0.90f, 0.92f, 0.96f, 1.0f), 0);
                const char* opacityOpts[4] = {"25%", "50%", "75%", "100%"};
                const int idx = fabsf(mSettings.opacity - 0.25f) < 0.12f ? 0 :
                                fabsf(mSettings.opacity - 0.50f) < 0.12f ? 1 :
                                fabsf(mSettings.opacity - 0.75f) < 0.12f ? 2 : 3;
                drawSegmentedGroup(leftX, y + 20.0f * uiScale, contentW, 32.0f * uiScale, opacityOpts, 4, idx);
            });
            renderRow(426.0f * uiScale, 54.0f * uiScale, [&](float y) {
                const float splitX = leftX + contentW * 0.58f;
                drawRoundRect(leftX, y + 4.0f * uiScale, splitX - 3.0f * uiScale, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.12f, 0.24f, 0.37f, 0.96f));
                drawRoundRect(leftX, y + 4.0f * uiScale, splitX - 3.0f * uiScale, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.35f, 0.62f, 0.87f, 0.65f));
                drawText("MOVER", (leftX + splitX) * 0.5f - 2.0f * uiScale,
                         y + 31.0f * uiScale, 0.31f * uiScale, rgba(0.94f, 0.97f, 1.0f, 1.0f), 1);
                drawRoundRect(splitX + 3.0f * uiScale, y + 4.0f * uiScale, leftX + contentW, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.22f, 0.19f, 0.16f, 0.96f));
                drawRoundRect(splitX + 3.0f * uiScale, y + 4.0f * uiScale, leftX + contentW, y + 48.0f * uiScale,
                              12.0f * uiScale, rgba(0.77f, 0.51f, 0.27f, 0.58f));
                drawText("RESTAURAR", (splitX + leftX + contentW) * 0.5f,
                         y + 31.0f * uiScale, 0.29f * uiScale, rgba(1.0f, 0.91f, 0.78f, 1.0f), 1);
            });
            renderRow(484.0f * uiScale, 36.0f * uiScale, [&](float y) {
                drawText("MANDO FISICO", leftX, y + 17.0f * uiScale,
                         0.38f * uiScale, rgba(0.42f, 0.88f, 0.76f, 1.0f), 0);
                drawText("Android HID · Bluetooth · USB OTG", leftX, y + 33.0f * uiScale,
                         0.25f * uiScale, rgba(0.70f, 0.75f, 0.84f, 0.88f), 0);
            });
            renderRow(522.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("ZONA MUERTA DEL STICK", leftX, y + 14.0f * uiScale,
                         0.32f * uiScale, rgba(0.90f, 0.92f, 0.96f, 1.0f), 0);
                const char* deadzoneOpts[4] = {"10%", "18%", "25%", "35%"};
                const float values[4] = {0.10f, 0.18f, 0.25f, 0.35f};
                int idx = 1;
                float best = 1.0f;
                for (int i = 0; i < 4; ++i) {
                    float diff = fabsf(mSettings.controllerDeadzone - values[i]);
                    if (diff < best) { best = diff; idx = i; }
                }
                drawSegmentedGroup(leftX, y + 20.0f * uiScale, contentW, 32.0f * uiScale, deadzoneOpts, 4, idx);
            });
            renderRow(578.0f * uiScale, 74.0f * uiScale, [&](float y) {
                drawText("SENSIBILIDAD IZQ.", leftX, y + 14.0f * uiScale,
                         0.27f * uiScale, rgba(0.90f, 0.92f, 0.96f, 1.0f), 0);
                drawText("SENSIBILIDAD DER.", rightX, y + 14.0f * uiScale,
                         0.27f * uiScale, rgba(0.90f, 0.92f, 0.96f, 1.0f), 0);
                const char* sensitivityOpts[4] = {"50%", "100%", "150%", "200%"};
                const float sensitivityValues[4] = {0.50f, 1.00f, 1.50f, 2.00f};
                auto sensitivityIndex = [&](float current) {
                    int best = 0;
                    float bestDiff = 10.0f;
                    for (int i = 0; i < 4; ++i) {
                        const float diff = fabsf(current - sensitivityValues[i]);
                        if (diff < bestDiff) { bestDiff = diff; best = i; }
                    }
                    return best;
                };
                drawSegmentedGroup(leftX, y + 20.0f * uiScale, halfW, 34.0f * uiScale,
                                   sensitivityOpts, 4, sensitivityIndex(mSettings.leftStickSensitivity));
                drawSegmentedGroup(rightX, y + 20.0f * uiScale, halfW, 34.0f * uiScale,
                                   sensitivityOpts, 4, sensitivityIndex(mSettings.rightStickSensitivity));
            });
            renderRow(658.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawBindCard(leftX, leftX + halfW, y, 0);
                drawBindCard(rightX, rightX + halfW, y, 1);
            });
            renderRow(714.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawBindCard(leftX, leftX + halfW, y, 2);
                drawBindCard(rightX, rightX + halfW, y, 3);
            });
            renderRow(770.0f * uiScale, 52.0f * uiScale, [&](float y) {
                drawBindCard(leftX, leftX + halfW, y, 4);
                drawBindCard(rightX, rightX + halfW, y, 5);
            });
            renderRow(826.0f * uiScale, 56.0f * uiScale, [&](float y) {
                drawRoundRect(leftX, y + 4.0f * uiScale, leftX + contentW, y + 50.0f * uiScale,
                              12.0f * uiScale, rgba(0.25f, 0.18f, 0.13f, 0.96f));
                drawRoundRect(leftX, y + 4.0f * uiScale, leftX + contentW, y + 50.0f * uiScale,
                              12.0f * uiScale, rgba(0.77f, 0.51f, 0.27f, 0.58f));
                drawText("RESTAURAR MANDO AUTOMATICO", leftX + contentW * 0.5f,
                         y + 31.0f * uiScale, 0.30f * uiScale, rgba(1.0f, 0.91f, 0.78f, 1.0f), 1);
            });
            renderRow(884.0f * uiScale, 56.0f * uiScale, [&](float y) {
                auto drawInvertCard = [&](float x0, float x1, const char* label, bool inverted) {
                    drawRoundRect(x0, y + 2.0f * uiScale, x1, y + 48.0f * uiScale,
                                  12.0f * uiScale, rgba(0.13f, 0.15f, 0.19f, 0.96f));
                    drawRoundRect(x0, y + 2.0f * uiScale, x1, y + 48.0f * uiScale,
                                  12.0f * uiScale, rgba(0.31f, 0.34f, 0.40f, 0.52f));
                    drawText(label, x0 + 10.0f * uiScale, y + 20.0f * uiScale,
                             0.27f * uiScale, rgba(0.91f, 0.93f, 0.97f, 1.0f), 0);
                    drawText(inverted ? "INVERTIDO" : "NORMAL", x0 + 10.0f * uiScale,
                             y + 38.0f * uiScale, 0.23f * uiScale,
                             inverted ? rgba(0.48f, 0.88f, 0.73f, 0.95f) : rgba(0.70f, 0.75f, 0.84f, 0.88f), 0);
                    drawM3Switch(x1 - swW - 8.0f * uiScale, y + 12.0f * uiScale, swW, swH, inverted);
                };
                drawInvertCard(leftX, leftX + halfW, "CAMARA EJE X", mSettings.cameraInvertX);
                drawInvertCard(rightX, rightX + halfW, "CAMARA EJE Y", mSettings.cameraInvertY);
            });
        }
        else if (mSettingsTab == 4) {
            auto renderRow = [&](float relY, float rowH, auto func) {
                float drawY = contentTop + relY - mDrawerScrollY;
                if (drawY + rowH >= contentTop - 12.0f && drawY <= contentBottom + 12.0f) {
                    func(drawY);
                }
            };
            const bool spanish = achievementsUseSpanish();
            const bool firstUnlocked = mAchievementFirstGameStarted;
            const int unlockedCount = firstUnlocked ? 1 : 0;
            const int remainingCount = 1 - unlockedCount;
            const int progressPercent = firstUnlocked ? 100 : 0;
            uint64_t playedSeconds = mAchievementTotalPlayedSeconds;
            if (mAchievementGameplayActive) {
                const int64_t nowMs = currentTimeMs();
                if (nowMs > mAchievementSessionStartMs) {
                    playedSeconds += static_cast<uint64_t>(nowMs - mAchievementSessionStartMs) / 1000;
                }
            }
            const uint64_t hours = playedSeconds / 3600;
            const uint64_t minutes = (playedSeconds % 3600) / 60;
            char playTimeText[64];
            if (spanish) {
                snprintf(playTimeText, sizeof(playTimeText), "%llu h %02llu min",
                         static_cast<unsigned long long>(hours),
                         static_cast<unsigned long long>(minutes));
            } else {
                snprintf(playTimeText, sizeof(playTimeText), "%llu hr %02llu min",
                         static_cast<unsigned long long>(hours),
                         static_cast<unsigned long long>(minutes));
            }
            char lastPlayedText[64];
            if (mAchievementLastPlayedEpochSeconds <= 0) {
                snprintf(lastPlayedText, sizeof(lastPlayedText), "%s",
                         spanish ? "Aun no has jugado" : "Not played yet");
            } else {
                const time_t lastPlayed = static_cast<time_t>(mAchievementLastPlayedEpochSeconds);
                struct tm local{};
                localtime_r(&lastPlayed, &local);
                static const char* monthsEn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                static const char* monthsEs[] = {"ene", "feb", "mar", "abr", "may", "jun",
                                                 "jul", "ago", "sep", "oct", "nov", "dic"};
                const int month = std::max(0, std::min(11, local.tm_mon));
                if (spanish) {
                    snprintf(lastPlayedText, sizeof(lastPlayedText), "%d %s %d - %02d:%02d",
                             local.tm_mday, monthsEs[month], local.tm_year + 1900,
                             local.tm_hour, local.tm_min);
                } else {
                    const int hour12 = (local.tm_hour % 12) == 0 ? 12 : local.tm_hour % 12;
                    snprintf(lastPlayedText, sizeof(lastPlayedText), "%s %d, %d - %d:%02d %s",
                             monthsEn[month], local.tm_mday, local.tm_year + 1900,
                             hour12, local.tm_min, local.tm_hour >= 12 ? "PM" : "AM");
                }
            }

            const float leftX = drawerX + padX;
            const float rightEdge = drawerX + drawerW - padX;
            const Color cardColor = rgba(0.13f, 0.15f, 0.19f, 0.98f);
            const Color borderColor = rgba(0.31f, 0.34f, 0.40f, 0.56f);
            const Color accent = rgba(0.42f, 0.88f, 0.76f, 1.0f);

            renderRow(8.0f * uiScale, 38.0f * uiScale, [&](float y) {
                drawText(spanish ? "LOGROS" : "ACHIEVEMENTS", leftX, y + 24.0f * uiScale,
                         0.40f * uiScale, accent, 0);
                drawText(spanish ? "Progreso local de esta instalacion" : "Local progress on this device",
                         leftX, y + 39.0f * uiScale, 0.25f * uiScale,
                         rgba(0.70f, 0.75f, 0.84f, 0.88f), 0);
            });
            renderRow(52.0f * uiScale, 92.0f * uiScale, [&](float y) {
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 88.0f * uiScale,
                              14.0f * uiScale, cardColor);
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 88.0f * uiScale,
                              14.0f * uiScale, borderColor);
                drawText(spanish ? "PROGRESO TOTAL" : "OVERALL PROGRESS",
                         leftX + 14.0f * uiScale, y + 23.0f * uiScale,
                         0.28f * uiScale, rgba(0.91f, 0.93f, 0.97f, 1.0f), 0);
                char progressLabel[48];
                snprintf(progressLabel, sizeof(progressLabel), "%d%%  -  %d / 1",
                         progressPercent, unlockedCount);
                drawText(progressLabel, rightEdge - 14.0f * uiScale, y + 23.0f * uiScale,
                         0.28f * uiScale, accent, 2);
                const float barX0 = leftX + 14.0f * uiScale;
                const float barX1 = rightEdge - 14.0f * uiScale;
                const float barY = y + 43.0f * uiScale;
                drawRoundRect(barX0, barY, barX1, barY + 12.0f * uiScale,
                              6.0f * uiScale, rgba(0.08f, 0.09f, 0.12f, 1.0f));
                if (progressPercent > 0) {
                    drawRoundRect(barX0, barY, barX0 + (barX1 - barX0) *
                                      (progressPercent / 100.0f), barY + 12.0f * uiScale,
                                  6.0f * uiScale, accent);
                }
                char countText[64];
                snprintf(countText, sizeof(countText), spanish
                             ? "Desbloqueados: %d | Restantes: %d | Total: 1"
                             : "Unlocked: %d | Remaining: %d | Total: 1",
                         unlockedCount, remainingCount);
                drawText(countText, leftX + 14.0f * uiScale, y + 75.0f * uiScale,
                         0.25f * uiScale, rgba(0.76f, 0.80f, 0.87f, 0.95f), 0);
            });
            renderRow(150.0f * uiScale, 124.0f * uiScale, [&](float y) {
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 118.0f * uiScale,
                              14.0f * uiScale, cardColor);
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 118.0f * uiScale,
                              14.0f * uiScale, borderColor);
                drawCircle(leftX + 29.0f * uiScale, y + 37.0f * uiScale,
                           17.0f * uiScale, firstUnlocked ? accent : rgba(0.30f, 0.33f, 0.39f, 1.0f));
                drawText("01", leftX + 29.0f * uiScale, y + 42.0f * uiScale,
                         0.25f * uiScale, rgba(0.06f, 0.08f, 0.10f, 1.0f), 1);
                drawText(spanish ? "Primera aventura" : "First Adventure",
                         leftX + 56.0f * uiScale, y + 31.0f * uiScale,
                         0.33f * uiScale, rgba(0.94f, 0.95f, 0.98f, 1.0f), 0);
                drawText(spanish ? "Inicia tu primera partida." : "Start your first game.",
                         leftX + 56.0f * uiScale, y + 53.0f * uiScale,
                         0.26f * uiScale, rgba(0.72f, 0.77f, 0.85f, 0.95f), 0);
                drawText(firstUnlocked
                             ? (spanish ? "DESBLOQUEADO" : "UNLOCKED")
                             : (spanish ? "BLOQUEADO" : "LOCKED"),
                         leftX + 56.0f * uiScale, y + 83.0f * uiScale,
                         0.24f * uiScale,
                         firstUnlocked ? accent : rgba(0.73f, 0.75f, 0.80f, 0.90f), 0);
            });
            renderRow(280.0f * uiScale, 112.0f * uiScale, [&](float y) {
                const float halfW = (contentW - 8.0f * uiScale) * 0.5f;
                const float rightX = leftX + halfW + 8.0f * uiScale;
                drawRoundRect(leftX, y + 2.0f * uiScale, leftX + halfW, y + 106.0f * uiScale,
                              14.0f * uiScale, cardColor);
                drawRoundRect(leftX, y + 2.0f * uiScale, leftX + halfW, y + 106.0f * uiScale,
                              14.0f * uiScale, borderColor);
                drawText(spanish ? "TIEMPO JUGADO" : "PLAY TIME",
                         leftX + 12.0f * uiScale, y + 25.0f * uiScale,
                         0.25f * uiScale, rgba(0.72f, 0.77f, 0.85f, 0.95f), 0);
                drawText(playTimeText, leftX + 12.0f * uiScale, y + 58.0f * uiScale,
                         0.35f * uiScale, rgba(0.94f, 0.95f, 0.98f, 1.0f), 0);

                drawRoundRect(rightX, y + 2.0f * uiScale, rightEdge, y + 106.0f * uiScale,
                              14.0f * uiScale, cardColor);
                drawRoundRect(rightX, y + 2.0f * uiScale, rightEdge, y + 106.0f * uiScale,
                              14.0f * uiScale, borderColor);
                drawText(spanish ? "ULTIMA PARTIDA" : "LAST PLAYED",
                         rightX + 12.0f * uiScale, y + 25.0f * uiScale,
                         0.25f * uiScale, rgba(0.72f, 0.77f, 0.85f, 0.95f), 0);
                drawText(lastPlayedText, rightX + 12.0f * uiScale, y + 58.0f * uiScale,
                         0.24f * uiScale, rgba(0.94f, 0.95f, 0.98f, 1.0f), 0);
            });
            renderRow(408.0f * uiScale, 66.0f * uiScale, [&](float y) {
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 62.0f * uiScale,
                              14.0f * uiScale, cardColor);
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 62.0f * uiScale,
                              14.0f * uiScale, borderColor);
                drawText(spanish ? "AVISOS DE LOGROS" : "ACHIEVEMENT NOTIFICATIONS",
                         leftX + 14.0f * uiScale, y + 27.0f * uiScale,
                         0.27f * uiScale, rgba(0.91f, 0.93f, 0.97f, 1.0f), 0);
                drawText(mSettings.achievementNotifications
                             ? (spanish ? "Activados" : "On")
                             : (spanish ? "Desactivados" : "Off"),
                         leftX + 14.0f * uiScale, y + 48.0f * uiScale,
                         0.23f * uiScale, rgba(0.70f, 0.75f, 0.84f, 0.92f), 0);
                drawM3Switch(rightEdge - swW - 14.0f * uiScale,
                             y + 17.0f * uiScale, swW, swH,
                             mSettings.achievementNotifications);
            });
            renderRow(482.0f * uiScale, 64.0f * uiScale, [&](float y) {
                const Color danger = rgba(0.38f, 0.13f, 0.16f, 0.98f);
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 60.0f * uiScale,
                              14.0f * uiScale, danger);
                drawRoundRect(leftX, y + 2.0f * uiScale, rightEdge, y + 60.0f * uiScale,
                              14.0f * uiScale, rgba(0.78f, 0.29f, 0.34f, 0.62f));
                drawText(spanish ? "BORRAR LOGROS OBTENIDOS" : "RESET EARNED ACHIEVEMENTS",
                         (leftX + rightEdge) * 0.5f, y + 38.0f * uiScale,
                         0.27f * uiScale, rgba(1.0f, 0.91f, 0.92f, 1.0f), 1);
            });
        }
        else {
            // Save Anywhere Tab Content
            auto renderRow = [&](float relY, float rowH, auto func) {
                float drawY = contentTop + relY - mDrawerScrollY;
                if (drawY + rowH >= contentTop - 12.0f && drawY <= contentBottom + 12.0f) {
                    func(drawY);
                }
            };

            // Section Header
            renderRow(4.0f * uiScale, 36.0f * uiScale, [&](float y) {
                drawText("Save & Load Anywhere", drawerX + padX, y + 16.0f * uiScale, 0.38f * uiScale, rgba(0.35f, 0.88f, 0.65f, 1.0f), 0);
                drawText("5 ranuras; los botones rapidos usan la ranura 4", drawerX + padX, y + 32.0f * uiScale, 0.27f * uiScale, rgba(0.68f, 0.75f, 0.88f, 0.85f), 0);
            });

            // Row 0: Quick Save / Load shortcuts (both use legacy slot 4).
            renderRow(44.0f * uiScale, 56.0f * uiScale, [&](float y) {
                float bW = (contentW - 12.0f * uiScale) * 0.5f;
                float btnY = y + 4.0f * uiScale;

                // Quick Save Card (Emerald green)
                float sX0 = drawerX + padX;
                float sX1 = sX0 + bW;
                drawRoundRect(sX0, btnY, sX1, btnY + 48.0f * uiScale, 14.0f * uiScale, rgba(0.05f, 0.40f, 0.30f, 0.95f));
                drawRoundRect(sX0, btnY, sX1, btnY + 48.0f * uiScale, 14.0f * uiScale, rgba(0.25f, 0.85f, 0.65f, 0.70f));
                drawCircle(sX0 + 20.0f * uiScale, btnY + 24.0f * uiScale, 6.0f * uiScale, rgba(0.35f, 0.98f, 0.75f, 1.0f));
                drawText("GUARDAR RAPIDO (4)", (sX0 + sX1) * 0.5f + 8.0f * uiScale, btnY + 29.0f * uiScale, 0.31f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);

                // Quick Load Card (Cyan blue)
                float lX0 = sX1 + 12.0f * uiScale;
                float lX1 = lX0 + bW;
                drawRoundRect(lX0, btnY, lX1, btnY + 48.0f * uiScale, 14.0f * uiScale, rgba(0.10f, 0.28f, 0.55f, 0.95f));
                drawRoundRect(lX0, btnY, lX1, btnY + 48.0f * uiScale, 14.0f * uiScale, rgba(0.35f, 0.68f, 0.98f, 0.70f));
                drawCircle(lX0 + 20.0f * uiScale, btnY + 24.0f * uiScale, 6.0f * uiScale, rgba(0.45f, 0.82f, 1.0f, 1.0f));
                drawText("CARGAR RAPIDO (4)", (lX0 + lX1) * 0.5f + 8.0f * uiScale, btnY + 29.0f * uiScale, 0.31f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);
            });

            // Row 1: Save slot selector.
            renderRow(112.0f * uiScale, 54.0f * uiScale, [&](float y) {
                drawText("GUARDAR EN", drawerX + padX, y + 14.0f * uiScale, 0.34f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                const char* slotOpts[5] = {"1", "2", "3", "4", "5"};
                drawSegmentedGroup(drawerX + padX, y + 20.0f * uiScale, contentW, 34.0f * uiScale, slotOpts, 5, mSaveSlot - 1);
            });

            // Row 2: Save the independently selected slot; require confirmation before replacing.
            renderRow(176.0f * uiScale, 52.0f * uiScale, [&](float y) {
                float btnY = y + 4.0f * uiScale;
                float sX0 = drawerX + padX;
                float sX1 = sX0 + contentW;
                drawRoundRect(sX0, btnY, sX1, btnY + 40.0f * uiScale, 12.0f * uiScale, rgba(0.06f, 0.38f, 0.28f, 0.95f));
                drawRoundRect(sX0, btnY, sX1, btnY + 40.0f * uiScale, 12.0f * uiScale, rgba(0.25f, 0.85f, 0.65f, 0.60f));
                char sTxt[32];
                bool confirming = mOverwriteConfirmSlot == mSaveSlot &&
                                  currentTimeMs() <= mOverwriteConfirmUntilMs;
                snprintf(sTxt, sizeof(sTxt), confirming ? "CONFIRMAR REEMPLAZO %d" : "GUARDAR RANURA %d", mSaveSlot);
                drawText(sTxt, (sX0 + sX1) * 0.5f, btnY + 25.0f * uiScale, 0.34f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);
            });

            // Row 3: Load selector and selected-slot availability.
            renderRow(238.0f * uiScale, 54.0f * uiScale, [&](float y) {
                const bool hasData = port_nand_save_slot_exists(mLoadSlot) != 0;
                drawText("CARGAR DE", drawerX + padX, y + 14.0f * uiScale, 0.34f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);
                char status[28];
                snprintf(status, sizeof(status), "RANURA %d: %s", mLoadSlot, hasData ? "GUARDADA" : "VACIA");
                drawText(status, drawerX + drawerW - padX, y + 14.0f * uiScale, 0.30f * uiScale, hasData ? rgba(0.35f, 0.88f, 0.65f, 1.0f) : rgba(0.65f, 0.68f, 0.74f, 0.9f), 2);
                const char* slotOpts[5] = {"1", "2", "3", "4", "5"};
                drawSegmentedGroup(drawerX + padX, y + 20.0f * uiScale, contentW, 34.0f * uiScale, slotOpts, 5, mLoadSlot - 1);
            });

            // Row 4: Load the selected slot. Empty slots stay visibly disabled.
            renderRow(300.0f * uiScale, 52.0f * uiScale, [&](float y) {
                const bool hasData = port_nand_save_slot_exists(mLoadSlot) != 0;
                float bX0 = drawerX + padX;
                float bX1 = bX0 + contentW;
                float btnY = y + 4.0f * uiScale;
                Color loadBase = hasData ? rgba(0.12f, 0.28f, 0.55f, 0.95f) : rgba(0.18f, 0.20f, 0.24f, 0.85f);
                Color loadLine = hasData ? rgba(0.35f, 0.65f, 0.98f, 0.60f) : rgba(0.35f, 0.38f, 0.43f, 0.45f);
                drawRoundRect(bX0, btnY, bX1, btnY + 40.0f * uiScale, 12.0f * uiScale, loadBase);
                drawRoundRect(bX0, btnY, bX1, btnY + 40.0f * uiScale, 12.0f * uiScale, loadLine);
                char loadText[32];
                snprintf(loadText, sizeof(loadText), "CARGAR RANURA %d", mLoadSlot);
                drawText(loadText, (bX0 + bX1) * 0.5f, btnY + 25.0f * uiScale, 0.34f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f), 1);
            });

            // Row 5: SD card save-data export/import.
            renderRow(362.0f * uiScale, 52.0f * uiScale, [&](float y) {
                float bW = (contentW - 12.0f * uiScale) * 0.5f;
                float btnY = y + 4.0f * uiScale;

                float eX0 = drawerX + padX;
                float eX1 = eX0 + bW;
                drawRoundRect(eX0, btnY, eX1, btnY + 40.0f * uiScale, 12.0f * uiScale, rgba(0.20f, 0.22f, 0.28f, 0.95f));
                drawRoundRect(eX0, btnY, eX1, btnY + 40.0f * uiScale, 12.0f * uiScale, rgba(0.40f, 0.50f, 0.70f, 0.60f));
                drawText("Exportar", (eX0 + eX1) * 0.5f, btnY + 25.0f * uiScale, 0.33f * uiScale, rgba(0.92f, 0.95f, 1.0f, 1.0f), 1);

                float iX0 = eX1 + 12.0f * uiScale;
                float iX1 = iX0 + bW;
                drawRoundRect(iX0, btnY, iX1, btnY + 40.0f * uiScale, 12.0f * uiScale, rgba(0.20f, 0.22f, 0.28f, 0.95f));
                drawRoundRect(iX0, btnY, iX1, btnY + 40.0f * uiScale, 12.0f * uiScale, rgba(0.40f, 0.50f, 0.70f, 0.60f));
                drawText("Importar", (iX0 + iX1) * 0.5f, btnY + 25.0f * uiScale, 0.33f * uiScale, rgba(0.92f, 0.95f, 1.0f, 1.0f), 1);
            });

            // Storage Details Card
            renderRow(424.0f * uiScale, 120.0f * uiScale, [&](float y) {
                drawRoundRect(drawerX + padX, y + 6.0f * uiScale, drawerX + drawerW - padX, y + 114.0f * uiScale, 14.0f * uiScale, rgba(0.12f, 0.15f, 0.20f, 0.95f));
                drawRoundRect(drawerX + padX, y + 6.0f * uiScale, drawerX + drawerW - padX, y + 114.0f * uiScale, 14.0f * uiScale, rgba(0.24f, 0.32f, 0.45f, 0.60f));
                drawText("NAND SAVE SYSTEM & STORAGE", drawerX + padX + 16.0f * uiScale, y + 26.0f * uiScale, 0.32f * uiScale, rgba(0.38f, 0.74f, 0.98f, 0.95f), 0);
                drawText("Location: /sdcard/GalaxyData/saves", drawerX + padX + 16.0f * uiScale, y + 46.0f * uiScale, 0.27f * uiScale, rgba(0.85f, 0.88f, 0.94f, 0.90f), 0);
                drawText("Guarda progreso; no captura toda la RAM.", drawerX + padX + 16.0f * uiScale, y + 66.0f * uiScale, 0.27f * uiScale, rgba(0.78f, 0.82f, 0.88f, 0.85f), 0);
                drawText("Al cargar valida la partida y reinicia la etapa.", drawerX + padX + 16.0f * uiScale, y + 86.0f * uiScale, 0.27f * uiScale, rgba(0.78f, 0.82f, 0.88f, 0.85f), 0);
                drawText("Compatible with Dolphin Wii emulator save files.", drawerX + padX + 16.0f * uiScale, y + 104.0f * uiScale, 0.26f * uiScale, rgba(0.60f, 0.70f, 0.85f, 0.75f), 0);
            });
        }

        // Scrollbar Thumb (rendered inside content area)
        if (maxScroll > 1.0f) {
            float trackH = contentBottom - contentTop;
            float thumbH = std::max(28.0f * uiScale, trackH * (trackH / totalContentH));
            float thumbY = contentTop + (mDrawerScrollY / maxScroll) * (trackH - thumbH);
            drawRoundRect(drawerX + drawerW - 5.0f * uiScale, thumbY, drawerX + drawerW - 2.0f * uiScale, thumbY + thumbH, 1.5f * uiScale, rgba(0.85f, 0.92f, 1.0f, 0.40f));
        }

        // ---------------------------------------------------------
        // B. Fixed Top Header & Tab Bar Mask Overlay
        // ---------------------------------------------------------
        drawRoundRect(drawerX - 60.0f, 0.0f, drawerX + drawerW, contentTop, 0.0f, rgba(0.106f, 0.114f, 0.137f, 1.0f));

        // Header Title & Cogwheel
        drawCogwheel(drawerX + padX + 16.0f * uiScale, 36.0f * uiScale, 15.0f * uiScale, rgba(0.659f, 0.780f, 0.980f, 1.0f), cogRot);
        drawText(mPlusEdition ? "GalaxyMobile+" : "Settings", drawerX + padX + 40.0f * uiScale, 44.0f * uiScale, 0.54f * uiScale, rgba(0.902f, 0.906f, 0.922f, 1.0f), 0);

        // Close [X] circular tonal button
        float closeBtnX = drawerX + drawerW - 36.0f * uiScale;
        float closeBtnY = 36.0f * uiScale;
        drawCircle(closeBtnX, closeBtnY, 17.0f * uiScale, rgba(0.188f, 0.196f, 0.224f, 0.95f));
        drawCloseIcon(closeBtnX, closeBtnY, 9.0f * uiScale, rgba(0.761f, 0.776f, 0.812f, 0.95f));

        {
            // Save slots are available in Standard; the mod tab remains Plus-only.
            float tabBoxY0 = 62.0f * uiScale;
            float tabBoxY1 = 98.0f * uiScale;
            drawRoundRect(drawerX + padX, tabBoxY0, drawerX + drawerW - padX, tabBoxY1, 18.0f * uiScale, rgba(0.149f, 0.160f, 0.190f, 0.95f));
            drawRoundRect(drawerX + padX, tabBoxY0, drawerX + drawerW - padX, tabBoxY1, 18.0f * uiScale, rgba(0.24f, 0.26f, 0.30f, 0.50f));

            const int visibleTabCount = mPlusEdition ? 5 : 4;
            const bool spanishAchievements = achievementsUseSpanish();
            const char* plusLabels[5] = {"General", "Mods",
                                         spanishAchievements ? "Controles" : "Controls",
                                         spanishAchievements ? "Ranuras" : "Saves",
                                         spanishAchievements ? "Logros" : "Achievements"};
            const int plusTabs[5] = {0, 1, 3, 2, 4};
            const char* standardLabels[4] = {"General",
                                             spanishAchievements ? "Controles" : "Controls",
                                             spanishAchievements ? "Ranuras" : "Saves",
                                             spanishAchievements ? "Logros" : "Achievements"};
            const int standardTabs[4] = {0, 3, 2, 4};
            const float tabSegW = contentW / (float)visibleTabCount;
            for (int i = 0; i < visibleTabCount; ++i) {
                const char* label = mPlusEdition ? plusLabels[i] : standardLabels[i];
                const int tab = mPlusEdition ? plusTabs[i] : standardTabs[i];
                const float x0 = drawerX + padX + (float)i * tabSegW;
                const float x1 = x0 + tabSegW;
                const bool active = (mSettingsTab == tab);
                if (active) {
                    drawRoundRect(x0 + 2.0f * uiScale, tabBoxY0 + 2.0f * uiScale,
                                  x1 - 2.0f * uiScale, tabBoxY1 - 2.0f * uiScale,
                                  16.0f * uiScale, rgba(0.14f, 0.28f, 0.48f, 0.95f));
                }
                const float labelScale = mPlusEdition ? 0.27f * uiScale : 0.30f * uiScale;
                const Color textColor = active ? rgba(1.0f, 1.0f, 1.0f, 1.0f)
                                               : rgba(0.68f, 0.71f, 0.77f, 0.75f);
                drawText(label, (x0 + x1) * 0.5f, tabBoxY0 + 23.0f * uiScale,
                         labelScale, textColor, 1);
            }

            // Divider below tab bar
            drawRoundRect(drawerX + padX, contentTop - 1.0f, drawerX + drawerW - padX, contentTop, 0.5f, rgba(0.267f, 0.278f, 0.310f, 0.45f));
        }

        // ---------------------------------------------------------
        // C. Fixed Bottom Bar Mask Overlay & [ Done ] Button
        // ---------------------------------------------------------
        drawRoundRect(drawerX - 60.0f, contentBottom, drawerX + drawerW, h, 0.0f, rgba(0.106f, 0.114f, 0.137f, 1.0f));
        drawRoundRect(drawerX + padX, contentBottom, drawerX + drawerW - padX, contentBottom + 1.0f, 0.5f, rgba(0.267f, 0.278f, 0.310f, 0.45f));

        float doneY = h - 48.0f * uiScale;
        float doneX0 = drawerX + padX;
        float doneX1 = drawerX + drawerW - padX;
        drawRoundRect(doneX0, doneY, doneX1, doneY + 38.0f * uiScale, 19.0f * uiScale, rgba(0.043f, 0.341f, 0.816f, 1.0f));
    }

    // -------------------------------------------------------------
    // Full-Screen In-Game Log Viewer Modal (Material 3 Dark Console)
    // -------------------------------------------------------------
    if (mLogViewerOpen) {
        refreshLogCache();

        // Scrim backdrop
        drawRoundRect(0, 0, w, h, 0.0f, rgba(0.0f, 0.0f, 0.0f, 0.85f));

        float modalPad = 24.0f * uiScale;
        float modalX0 = modalPad;
        float modalY0 = modalPad;
        float modalX1 = w - modalPad;
        float modalY1 = h - modalPad;

        // Modal container
        drawRoundRect(modalX0, modalY0, modalX1, modalY1, 16.0f * uiScale, rgba(0.08f, 0.09f, 0.12f, 0.98f));
        drawRoundRect(modalX0, modalY0, modalX1, modalY1, 16.0f * uiScale, rgba(0.25f, 0.35f, 0.50f, 0.60f));

        // Header and error summary
        float headerH = 46.0f * uiScale;
        drawText("REGISTRO DE DIAGNOSTICO", modalX0 + 20.0f * uiScale, modalY0 + 29.0f * uiScale, 0.38f * uiScale, rgba(0.38f, 0.74f, 0.98f, 1.0f), 0);
        char summary[128];
        snprintf(summary, sizeof(summary), "%d/%d lineas  |  %d errores  |  %d avisos",
                 (int)mLogFilteredLines.size(), mLogLineCount,
                 mLogErrorCount, mLogWarningCount);
        drawText(summary, modalX0 + 20.0f * uiScale, modalY0 + headerH + 18.0f * uiScale,
                 0.30f * uiScale, rgba(0.68f, 0.73f, 0.82f, 0.95f), 0);

        // Close button [X]
        float closeX = modalX1 - 24.0f * uiScale;
        float closeY = modalY0 + 23.0f * uiScale;
        drawCircle(closeX, closeY, 16.0f * uiScale, rgba(0.20f, 0.22f, 0.28f, 0.95f));
        drawCloseIcon(closeX, closeY, 9.0f * uiScale, rgba(1.0f, 1.0f, 1.0f, 1.0f));

        // Separator line
        drawLine(modalX0, modalY0 + headerH, modalX1, modalY0 + headerH, 1.2f * uiScale, rgba(0.25f, 0.28f, 0.35f, 0.60f));

        const char* filterLabels[6] = {"TODO", "PARTIDAS", "CARGAS", "GRAFICOS", "AUDIO", "ERRORES"};
        const float filterX0 = modalX0 + 12.0f * uiScale;
        const float filterX1 = modalX1 - 12.0f * uiScale;
        const float filterY0 = modalY0 + headerH + 26.0f * uiScale;
        const float filterH = 24.0f * uiScale;
        const float filterW = (filterX1 - filterX0) / 6.0f;
        for (int i = 0; i < 6; ++i) {
            const float x0 = filterX0 + i * filterW + 2.0f * uiScale;
            const float x1 = filterX0 + (i + 1) * filterW - 2.0f * uiScale;
            const bool selected = mLogFilter == i;
            drawRoundRect(x0, filterY0, x1, filterY0 + filterH, 6.0f * uiScale,
                          selected ? rgba(0.12f, 0.39f, 0.51f, 1.0f)
                                   : rgba(0.14f, 0.16f, 0.21f, 0.96f));
            drawRoundRect(x0, filterY0, x1, filterY0 + filterH, 6.0f * uiScale,
                          selected ? rgba(0.36f, 0.80f, 0.89f, 0.45f)
                                   : rgba(0.30f, 0.34f, 0.42f, 0.55f));
            drawText(filterLabels[i], (x0 + x1) * 0.5f, filterY0 + 16.5f * uiScale,
                     0.20f * uiScale,
                     selected ? rgba(0.94f, 1.0f, 1.0f, 1.0f)
                              : rgba(0.70f, 0.75f, 0.83f, 0.95f), 1);
        }

        const float footerY0 = modalY1 - 58.0f * uiScale;
        float lineY = filterY0 + filterH + 8.0f * uiScale;
        const float lineStep = 15.0f * uiScale;
        const int visibleLines = std::max(1, (int)((footerY0 - lineY - 8.0f * uiScale) / lineStep));
        const int maxScroll = std::max(0, (int)mLogFilteredLines.size() - visibleLines);
        const int scrollLines = std::max(0, std::min(maxScroll, (int)mLogScrollY));
        const int endLine = std::max(0, (int)mLogFilteredLines.size() - scrollLines);
        const int startLine = std::max(0, endLine - visibleLines);
        if (!mLogFilteredLines.empty()) {
            for (int i = startLine; i < endLine && lineY < footerY0 - 8.0f * uiScale; ++i) {
                const LogLineRef& ref = mLogFilteredLines[(size_t)i];
                const std::string fullLine = mLogText.substr(ref.offset, ref.length);
                std::string line = fullLine.substr(0, std::min<size_t>(fullLine.size(), 150));
                if (fullLine.size() > 150 && line.size() >= 3) line.replace(line.size() - 3, 3, "...");
                Color lineColor = rgba(0.85f, 0.88f, 0.92f, 0.95f);
                if (isDiagnosticError(line.c_str(), line.size())) {
                    lineColor = rgba(1.0f, 0.48f, 0.44f, 1.0f);
                } else if (isDiagnosticWarning(line.c_str(), line.size())) {
                    lineColor = rgba(1.0f, 0.78f, 0.38f, 1.0f);
                }
                drawText(line.c_str(), modalX0 + 18.0f * uiScale, lineY,
                         0.27f * uiScale, lineColor, 0);
                lineY += lineStep;
            }
        } else {
            drawText("Sin registros en esta categoria.", modalX0 + 20.0f * uiScale,
                     lineY, 0.34f * uiScale, rgba(0.60f, 0.65f, 0.72f, 0.85f), 0);
        }

        // Copy action is always visible and copies the complete recent ring, not
        // just the lines currently visible in the modal.
        float copyX0 = modalX0 + 20.0f * uiScale;
        float copyX1 = modalX1 - 20.0f * uiScale;
        float copyY1 = modalY1 - 12.0f * uiScale;
        drawRoundRect(copyX0, footerY0, copyX1, copyY1, 12.0f * uiScale,
                      rgba(0.10f, 0.36f, 0.44f, 0.98f));
        drawRoundRect(copyX0, footerY0, copyX1, copyY1, 12.0f * uiScale,
                      rgba(0.35f, 0.82f, 0.95f, 0.52f));
        drawText("COPIAR REGISTRO COMPLETO", (copyX0 + copyX1) * 0.5f,
                 (footerY0 + copyY1) * 0.5f + 5.0f * uiScale,
                 0.34f * uiScale, rgba(0.93f, 0.98f, 1.0f, 1.0f), 1);
    }

    // -------------------------------------------------------------
    // Save Anywhere Notification Toast (Floating Pill)
    // -------------------------------------------------------------
    if (mSaveToastTimer > 0.0f) {
        float toastAlpha = std::min(1.0f, mSaveToastTimer * 2.0f);
        float toastW = 320.0f * uiScale;
        float toastH = 44.0f * uiScale;
        float toastX = (w - toastW) * 0.5f;
        float toastY = 56.0f * uiScale;

        // Container: dark emerald frosted pill
        drawRoundRect(toastX, toastY, toastX + toastW, toastY + toastH, 22.0f * uiScale, rgba(0.04f, 0.15f, 0.11f, 0.95f * toastAlpha));
        drawRoundRect(toastX, toastY, toastX + toastW, toastY + toastH, 22.0f * uiScale, rgba(0.20f, 0.82f, 0.55f, 0.85f * toastAlpha));

        // Green checkmark indicator circle
        drawCircle(toastX + 24.0f * uiScale, toastY + toastH * 0.5f, 9.0f * uiScale, rgba(0.18f, 0.85f, 0.55f, toastAlpha));
        drawCircle(toastX + 24.0f * uiScale, toastY + toastH * 0.5f, 4.0f * uiScale, rgba(1.0f, 1.0f, 1.0f, toastAlpha));

        drawText(mToastMessage.empty() ? "Game Progress Saved Anywhere!" : mToastMessage.c_str(), toastX + 42.0f * uiScale, toastY + toastH * 0.5f + 5.0f * uiScale, 0.35f * uiScale, rgba(0.95f, 1.0f, 0.97f, toastAlpha), 0);
    }

    if (mSettings.achievementNotifications &&
        currentTimeMs() < mAchievementToastUntilMs) {
        const float toastW = std::min(450.0f * uiScale, w - 24.0f * uiScale);
        const float toastH = 44.0f * uiScale;
        const float toastX = (w - toastW) * 0.5f;
        const float toastY = 8.0f * uiScale;
        drawRoundRect(toastX, toastY, toastX + toastW, toastY + toastH,
                      18.0f * uiScale, rgba(0.05f, 0.14f, 0.13f, 0.97f));
        drawRoundRect(toastX, toastY, toastX + toastW, toastY + toastH,
                      18.0f * uiScale, rgba(0.35f, 0.88f, 0.75f, 0.90f));
        drawCircle(toastX + 23.0f * uiScale, toastY + toastH * 0.5f,
                   7.0f * uiScale, rgba(0.40f, 0.93f, 0.79f, 1.0f));
        drawText(mAchievementToastMessage.c_str(), toastX + toastW * 0.5f,
                 toastY + toastH * 0.5f + 5.0f * uiScale,
                 0.29f * uiScale, rgba(0.96f, 1.0f, 0.98f, 1.0f), 1);
    }

    if (mAchievementClearConfirm) {
        const float dialogW = std::min(470.0f * uiScale, w - 32.0f * uiScale);
        const float dialogH = 190.0f * uiScale;
        const float dialogX0 = (w - dialogW) * 0.5f;
        const float dialogX1 = dialogX0 + dialogW;
        const float dialogY0 = (h - dialogH) * 0.5f;
        const float dialogY1 = dialogY0 + dialogH;
        const float buttonY0 = dialogY1 - 60.0f * uiScale;
        const float buttonY1 = dialogY1 - 14.0f * uiScale;
        const float splitX = (dialogX0 + dialogX1) * 0.5f;
        drawRoundRect(0.0f, 0.0f, w, h, 0.0f, rgba(0.0f, 0.0f, 0.0f, 0.72f));
        drawRoundRect(dialogX0, dialogY0, dialogX1, dialogY1,
                      20.0f * uiScale, rgba(0.12f, 0.14f, 0.18f, 1.0f));
        drawRoundRect(dialogX0, dialogY0, dialogX1, dialogY1,
                      20.0f * uiScale, rgba(0.39f, 0.43f, 0.50f, 0.75f));
        const bool spanish = achievementsUseSpanish();
        drawText(spanish ? "¿Borrar logros obtenidos?" : "Reset earned achievements?",
                 (dialogX0 + dialogX1) * 0.5f, dialogY0 + 39.0f * uiScale,
                 0.34f * uiScale, rgba(0.96f, 0.97f, 1.0f, 1.0f), 1);
        drawText(spanish ? "El tiempo jugado y la ultima partida se conservan."
                         : "Playtime and last played will be kept.",
                 (dialogX0 + dialogX1) * 0.5f, dialogY0 + 82.0f * uiScale,
                 0.25f * uiScale, rgba(0.74f, 0.78f, 0.86f, 1.0f), 1);
        drawRoundRect(dialogX0 + 16.0f * uiScale, buttonY0,
                      splitX - 6.0f * uiScale, buttonY1,
                      12.0f * uiScale, rgba(0.23f, 0.26f, 0.32f, 1.0f));
        drawRoundRect(splitX + 6.0f * uiScale, buttonY0,
                      dialogX1 - 16.0f * uiScale, buttonY1,
                      12.0f * uiScale, rgba(0.63f, 0.22f, 0.27f, 1.0f));
        drawText(spanish ? "CANCELAR" : "CANCEL",
                 (dialogX0 + splitX) * 0.5f, buttonY0 + 29.0f * uiScale,
                 0.25f * uiScale, rgba(0.93f, 0.95f, 0.99f, 1.0f), 1);
        drawText(spanish ? "BORRAR" : "RESET",
                 (splitX + dialogX1) * 0.5f, buttonY0 + 29.0f * uiScale,
                 0.25f * uiScale, rgba(1.0f, 0.94f, 0.94f, 1.0f), 1);
    }

}

void UIOverlay::render(int screenW, int screenH) {
    if (!init()) return;

    std::vector<UIVertex> verts;
    generateVertices(screenW, screenH, verts);
    if (verts.empty()) return;

    float w = (float)screenW;
    float h = (float)screenH;

    // -------------------------------------------------------------
    // Single GLES Draw Call
    // -------------------------------------------------------------
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(mProg);

    float proj[16] = {
        2.0f / w, 0.0f,      0.0f, 0.0f,
        0.0f,     -2.0f / h, 0.0f, 0.0f,
        0.0f,     0.0f,     -1.0f, 0.0f,
        -1.0f,    1.0f,      0.0f, 1.0f
    };
    GLint uProjLoc = glGetUniformLocation(mProg, "uProj");
    glUniformMatrix4fv(uProjLoc, 1, GL_FALSE, proj);

    GLint uFontLoc = glGetUniformLocation(mProg, "uFontTex");
    glUniform1i(uFontLoc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mFontTex);

    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(UIVertex), verts.data(), GL_STREAM_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());

    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

void UIOverlay::setEditMode(bool active) {
    mEditLayoutMode = active;
    if (active) {
        mSettingsOpen = false;
    }
    mDirty = true;
}

void UIOverlay::clearLayoutOffsets() {
    mSettings.stickOffsetX = 0.0f;
    mSettings.stickOffsetY = 0.0f;
    mSettings.buttonOffsetX = 0.0f;
    mSettings.buttonOffsetY = 0.0f;
    mSettings.btnAOffsetX = 0.0f;
    mSettings.btnAOffsetY = 0.0f;
    mSettings.btnBOffsetX = 0.0f;
    mSettings.btnBOffsetY = 0.0f;
    mSettings.btnZOffsetX = 0.0f;
    mSettings.btnZOffsetY = 0.0f;
    mSettings.btnSpinOffsetX = 0.0f;
    mSettings.btnSpinOffsetY = 0.0f;
    mSettings.pauseOffsetX = 0.0f;
    mSettings.pauseOffsetY = 0.0f;
    mSettings.dpadOffsetX = 0.0f;
    mSettings.dpadOffsetY = 0.0f;
}

void UIOverlay::resetLayoutOffsets() {
    clearLayoutOffsets();
    if (!mSavePath.empty()) {
        mSettings.save(mSavePath);
    }
    mDirty = true;
}

void UIOverlay::beginControllerCapture(int action) {
    if (!controllerBinding(mSettings, action)) return;
    mControllerCaptureAction = action;
    LOGI("controller: waiting for a physical key assignment to %s", controllerActionName(action));
    showToast(std::string("Pulsa el boton del mando para ") + controllerActionName(action));
    mDirty = true;
}

bool UIOverlay::captureControllerKey(int keyCode, bool down) {
    if (mControllerCaptureAction < 0 || !down) return false;
    if (keyCode == AKEYCODE_BACK || keyCode == AKEYCODE_ESCAPE) {
        LOGI("controller: key capture cancelled");
        mControllerCaptureAction = -1;
        showToast("Asignacion cancelada");
        mDirty = true;
        return true;
    }

    const bool buttonKey = (keyCode >= AKEYCODE_BUTTON_A && keyCode <= AKEYCODE_BUTTON_MODE) ||
                           (keyCode >= AKEYCODE_BUTTON_1 && keyCode <= AKEYCODE_BUTTON_16);
    const bool dpadKey = keyCode == AKEYCODE_DPAD_UP || keyCode == AKEYCODE_DPAD_DOWN ||
                         keyCode == AKEYCODE_DPAD_LEFT || keyCode == AKEYCODE_DPAD_RIGHT ||
                         keyCode == AKEYCODE_DPAD_CENTER;
    if (!buttonKey && !dpadKey) return false;

    int* target = controllerBinding(mSettings, mControllerCaptureAction);
    if (!target) {
        mControllerCaptureAction = -1;
        return true;
    }

    // Keep mappings unique. A physical button must not trigger two actions
    // after a remap; the displaced action returns to its automatic default.
    for (int i = 0; i < 6; ++i) {
        int* other = controllerBinding(mSettings, i);
        if (other && other != target && *other == keyCode) *other = -1;
    }
    const int action = mControllerCaptureAction;
    *target = keyCode;
    mControllerCaptureAction = -1;
    if (!mSavePath.empty()) mSettings.save(mSavePath);
    LOGI("controller: mapped %s to Android keycode %d (%s)",
         controllerActionName(action), keyCode, controllerKeyName(keyCode).c_str());
    showToast(std::string(controllerActionName(action)) + " asignado a " + controllerKeyName(keyCode));
    mDirty = true;
    return true;
}

bool UIOverlay::exportSave() {
    std::string internalPath = mSavePath;
    size_t lastSlash = internalPath.rfind('/');
    if (lastSlash == std::string::npos) return false;
    std::string saveRoot = internalPath.substr(0, lastSlash);

    // SMG official save name is GameData.bin (also check save.bin and all regions)
    const char* titles[] = {"524d4750", "524d4745", "524d474a", "524d474b"};
    const char* fileNames[] = {"GameData.bin", "save.bin"};
    std::string srcSave;
    FILE* in = nullptr;

    for (const char* t : titles) {
        for (const char* fn : fileNames) {
            std::string candidate = saveRoot + "/title/00010000/" + t + "/data/" + fn;
            in = fopen(candidate.c_str(), "rb");
            if (in) { srcSave = candidate; break; }
        }
        if (in) break;
    }
    if (!in) {
        for (const char* fn : fileNames) {
            std::string candidate = saveRoot + "/" + fn;
            in = fopen(candidate.c_str(), "rb");
            if (in) { srcSave = candidate; break; }
        }
    }
    if (!in) {
        // Also check external backup mirror
        std::string extCandidate = "/sdcard/GalaxyData/saves/GameData.bin";
        in = fopen(extCandidate.c_str(), "rb");
        if (in) srcSave = extCandidate;
    }
    if (!in) {
        showToast("No save data found yet (play & save first)!");
        return false;
    }

    // Write to both GalaxyAndroid_save.bin and GameData.bin in Downloads
    mkdir("/sdcard/Download", 0770);
    std::string dstSave1 = "/sdcard/Download/GalaxyAndroid_save.bin";
    std::string dstSave2 = "/sdcard/Download/GameData.bin";

    FILE* out1 = fopen(dstSave1.c_str(), "wb");
    FILE* out2 = fopen(dstSave2.c_str(), "wb");
    if (!out1 && !out2) {
        fclose(in);
        showToast("Cannot write to Downloads folder!");
        return false;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (out1) fwrite(buf, 1, n, out1);
        if (out2) fwrite(buf, 1, n, out2);
    }
    fclose(in);
    if (out1) fclose(out1);
    if (out2) fclose(out2);

    showToast("Exported to Download/GalaxyAndroid_save.bin!");
    return true;
}

bool UIOverlay::importSave() {
    std::string internalPath = mSavePath;
    size_t lastSlash = internalPath.rfind('/');
    if (lastSlash == std::string::npos) return false;
    std::string saveRoot = internalPath.substr(0, lastSlash);

    // Look for save in Downloads
    const char* downloadCandidates[] = {
        "/sdcard/Download/GalaxyAndroid_save.bin",
        "/sdcard/Download/GameData.bin",
        "/sdcard/Download/save.bin"
    };
    FILE* in = nullptr;
    for (const char* path : downloadCandidates) {
        in = fopen(path, "rb");
        if (in) break;
    }
    if (!in) {
        showToast("Put GalaxyAndroid_save.bin in Downloads!");
        return false;
    }

    std::vector<uint8_t> data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        data.insert(data.end(), buf, buf + n);
    }
    fclose(in);

    if (data.empty()) {
        showToast("Save file in Downloads is empty!");
        return false;
    }

    // Import into all region directories (PAL, NTSC-U, NTSC-J, NTSC-K)
    const char* titles[] = {"524d4750", "524d4745", "524d474a", "524d474b"};
    mkdir((saveRoot + "/title").c_str(), 0770);
    mkdir((saveRoot + "/title/00010000").c_str(), 0770);
    for (const char* t : titles) {
        std::string dir = saveRoot + "/title/00010000/" + t + "/data";
        mkdir((saveRoot + "/title/00010000/" + t).c_str(), 0770);
        mkdir(dir.c_str(), 0770);
        FILE* out = fopen((dir + "/GameData.bin").c_str(), "wb");
        if (out) {
            fwrite(data.data(), 1, data.size(), out);
            fclose(out);
        }
        FILE* outOld = fopen((dir + "/save.bin").c_str(), "wb");
        if (outOld) {
            fwrite(data.data(), 1, data.size(), outOld);
            fclose(outOld);
        }
    }
    // Also save to external mirror in GalaxyData/saves
    mkdir("/sdcard/GalaxyData", 0770);
    mkdir("/sdcard/GalaxyData/saves", 0770);
    FILE* outExt = fopen("/sdcard/GalaxyData/saves/GameData.bin", "wb");
    if (outExt) {
        fwrite(data.data(), 1, data.size(), outExt);
        fclose(outExt);
    }

    showToast("Imported save data successfully!");
    return true;
}

}  // namespace mobile_ui
