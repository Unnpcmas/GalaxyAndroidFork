#pragma once

#include <android/input.h>
#include <stdint.h>
#include <string>
#include "port/input.h"

#include <vector>
#include "achievements.h"

extern "C" int port_android_choose_mod_zip(const char* modsDirectory);

namespace mobile_ui {

// In-shader vertex for 2D batching (used by both GLES 3.2 and Vulkan 1.0)
struct UIVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
    float mode;   // 0 = rect, 1 = circle/ring, 2 = font glyph, 3 = cogwheel, 4 = close 'X', etc.
    float param;  // ring: inner ratio; rect: corner radius ratio; cogwheel: rot angle
    float aspect; // rect aspect ratio (w/h or h/w)
    float extra;  // rect: 0 = horiz/square, 1 = vert
};

struct Settings {
    bool showControls = true;
    // Use the device's real ultrawide aspect by default (Hor+); this expands
    // horizontal view instead of stretching or cropping the 3D image.
    bool fullscreen = true;
    int screenScaleIndex = 2; // 0 = 85%, 1 = 95%, 2 = fit available area
    float opacity = 0.65f;
    float scale = 1.0f;
    bool dynamicStick = true;
    float stickOffsetX = 0.0f;
    float stickOffsetY = 0.0f;
    float buttonOffsetX = 0.0f;
    float buttonOffsetY = 0.0f;
    // Per-button offsets on top of the shared button cluster offset, so A, B,
    // Z and SPIN can be pulled apart instead of only moving as one pad.
    float btnAOffsetX = 0.0f;
    float btnAOffsetY = 0.0f;
    float btnBOffsetX = 0.0f;
    float btnBOffsetY = 0.0f;
    float btnZOffsetX = 0.0f;
    float btnZOffsetY = 0.0f;
    float btnSpinOffsetX = 0.0f;
    float btnSpinOffsetY = 0.0f;
    float pauseOffsetX = 0.0f;
    float pauseOffsetY = 0.0f;
    bool showStick = true;
    bool showButtonA = true;
    bool showButtonB = true;
    bool showButtonZ = true;
    bool showButtonSpin = true;
    bool showPauseButton = true;
    bool showDpad = true;
    bool dpadOnRight = false;
    float dpadOffsetX = 0.0f;
    float dpadOffsetY = 0.0f;
    // -1 keeps the standard Android gamepad mapping; nonnegative values are
    // Android key codes captured from a physical controller.
    int controllerKeyA = -1;
    int controllerKeyB = -1;
    int controllerKeyC = -1;
    int controllerKeyZ = -1;
    int controllerKeySpin = -1;
    int controllerKeyPause = -1;
    float controllerDeadzone = 0.18f;
    float leftStickSensitivity = 1.0f;
    float rightStickSensitivity = 1.0f;
    bool cameraInvertX = true;
    bool cameraInvertY = true;

    // NVIDIA Shield & Remaster Mods
    bool modShieldTextures = true;     // NVIDIA Shield 1080p AI Remastered Textures
    bool modShieldCutscenes = true;    // 60 FPS Remastered Cutscenes
    bool modShieldControls = true;     // NVIDIA Shield Calibrated Controller Curves
    bool modClassicGamepadUI = true;   // ABXY Gamepad Button Sheets
    bool modRiivolution = true;        // /sdcard/GalaxyData/mods Riivolution patches

    // Advanced Mods (off by default)
    bool modFly = false;
    // Second (camera) joystick, motion camera, adaptive quality and FOV
    bool showCamStick = true;
    float camStickScale = 1.0f;      // 0.8 / 1.0 / 1.25
    float camStickOpacity = 0.75f;   // 0.25 .. 1.0
    float camStickSensitivity = 1.0f; // touch camera stick gain 0.5 .. 2.0
    float camStickOffsetX = 0.0f;
    float camStickOffsetY = 0.0f;
    bool showCamReset = true;        // the "reset camera" button
    float camResetScale = 1.0f;      // 0.8 / 1.0 / 1.25
    float camResetOpacity = 0.75f;   // 0.25 .. 1.0
    float camResetOffsetX = 0.0f;    // from its default place beside the camera joystick
    float camResetOffsetY = 0.0f;
    bool gyroCamera = false;         // move the phone to turn the camera (off by default)
    float gyroCameraSens = 1.0f;     // 0.6 soft / 1.0 normal / 1.6 fast
    bool adaptiveQuality = true;     // lower FXAA while the frame rate stays low
    int fovOffset = 0;               // degrees added to the game's FOV (0 = untouched)
    bool modLuigi = false;      // saved: play as Luigi (applies on the next stage load)
    bool modFreeCamera = false; // runtime only (not saved): free camera, see freecam.h
    bool modSkipCutscenes = false;
    bool modInfiniteHealth = false;
    bool modInfiniteLives = false;
    bool modInfiniteStarBits = false;

    // Difficulty (Mods tab). Damage values are multipliers of the hit's health cost.
    int diffEnemyDamage = 1;       // 1..4: health lost per hit from enemies and hazards
    int diffBossDamage = 1;        // 1..4: same, while fighting in a boss galaxy
    bool diffOneHit = false;       // any damage is lethal
    bool diffNoHeal = false;       // hearts, coins and mushrooms do not restore health
    bool diffMaxHealth3 = false;   // the Life Mushroom does not raise the health meter to 6

    // Randomizer (Mods tab): a seeded shuffle; the same seed always gives the same game.
    bool randoEnabled = false;
    int randoSeed = 123456;        // 0..999999, shown as six digits
    bool randoGalaxies = true;     // which galaxy is behind each galaxy door
    bool randoStars = true;        // which mission is loaded when a mission is picked
    bool randoEnemies = true;      // enemies swapped inside families that behave alike
    bool randoItems = true;        // mushrooms swapped with each other
    bool randoPowerUps = false;    // power-ups swapped too (can get you stuck; never in challenge mode)
    bool randoChallenge = false;   // keeps the game completable: forbids swapping power-ups

    // Log (General tab): false keeps only ERROR/FATAL lines.
    bool logEnabled = true;

    // Game Speed Multiplier (1 = 1x, 2 = 1.5x, 3 = 2x, 4 = 3x, 5 = 4x)
    int gameSpeed = 1;

    // In-game Language (0 = Auto, 1 = English, 2 = Spanish, 3 = French, 4 = German, 5 = Italian)
    int languageChoice = 0;
    bool achievementNotifications = true;

    // Render quality (0 = 480p, 1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 3840px 4K long edge)
    int renderScale = 2;
    // Post-process FXAA quality level (0 = off, 1..12 = progressively softer edge filtering).
    int antialiasingLevel = 0;

    // Haptic feedback (off by default)
    bool haptics = false;

    // FPS configuration
    int targetFps = 60;  // 60 (native); 30 only through Battery Saver
    bool adaptiveFps = true;
    bool batterySaver = false; // 30 FPS thermal protection & power save mode
    bool gyroAiming = false;   // Gyroscope / accelerometer Star Pointer motion aiming
    bool gyroInvertX = false;  // Invert horizontal gyro axis
    bool gyroInvertY = false;  // Invert vertical gyro axis

    bool fullTouchMode = true;
    int perfHudMode = 0; // 0 = Off, 1 = FPS, 2 = Full Hardware Stats
    int graphicsBackend = 0; // 0 = OpenGL ES 3.2 (compatibility default), 1 = Vulkan 1.0
    bool relativePointer = false; // Relative trackpad-style Star Pointer aiming (Issue #11)
    int screenOrientationChoice = 0; // 0 = Auto Sensor Landscape, 1 = Landscape, 2 = Reverse Landscape (Issues #18 & #19)

    void load(const std::string& path);
    void save(const std::string& path) const;
    void logMods(const char* why) const;  // every mod and difficulty value, one line each
};

class UIOverlay {
public:
    static UIOverlay& get();

    bool init();
    void setSavePath(const std::string& path);
    void setAchievementsPath(const std::string& path);
    void setGameplayActive(bool active);
    bool isGameplayActive() const { return mAchievementGameplayActive; }
    void pauseAchievementSession();
    void resumeAchievementSession();
    void tickAchievementSession();
    // Facts from the game (see port_mobile_achievement_* in mobile_ui.cpp).
    void achievementPoll(int userFile, int stars, int starMax, int grandStars, int starBits, int coins, int lives);
    void achievementStage(const char* stage, bool luigi);
    void achievementEvent(const char* kind, const char* a, const char* b, int n);

    // Per-frame tick to handle pulse buttons, auto-release, and hardware metrics
    void update(PortPadState& pad, int& spinFrames);

    // Current active buttons pressed on touchscreen
    uint32_t getTouchButtons() const;

    // Current virtual stick state
    bool isStickActive() const { return mStickPointerId >= 0; }
    float getTouchStickX() const { return (mStickPointerId >= 0) ? mTouchStickX : 0.0f; }
    float getTouchStickY() const { return (mStickPointerId >= 0) ? mTouchStickY : 0.0f; }

    // Camera (right) touch joystick: -1..1, right / up positive; 0 when released.
    float getCamStickX() const { return (mCamStickPointerId >= 0) ? mCamStickX : 0.0f; }
    float getCamStickY() const { return (mCamStickPointerId >= 0) ? mCamStickY : 0.0f; }
    // Centre and radius (pixels) of the camera joystick: shared by the touch
    // handling, the drawing and the layout editor; always inside the screen.
    void camStickGeometry(int screenW, int screenH, float& cx, float& cy, float& r) const;
    // The "reset camera" button sits beside the camera joystick (same rules).
    void camResetGeometry(int screenW, int screenH, float& cx, float& cy, float& r) const;

    // Processes an Android motion or key event.
    // Returns true if consumed (e.g., settings menu or virtual controls).
    bool handleInput(const AInputEvent* event, PortPadState& pad, int& spinFrames,
                     int screenW, int screenH, int vpX, int vpY, int vpW, int vpH);

    // Generate 2D quad geometry for Vulkan and OpenGL renderers
    void generateVertices(int screenW, int screenH, std::vector<UIVertex>& outVerts);

    // Retrieves font atlas dimensions and 8-bit coverage data
    static void getFontAtlas(int& width, int& height, std::vector<uint8_t>& coverage);

    // Render the overlay in OpenGL ES 3.2
    void render(int screenW, int screenH);

    // Call when a frame is presented to display to track true presented FPS
    void reportFramePresented();
    // Called once per NEW game frame (not for repeated or UI-only presents).
    void reportGameFramePresented();

    bool needsRedraw();
    bool isSettingsOpen() const { return mSettingsOpen || mDrawerAnim > 0.01f; }
    void closeSettings();
    void triggerSaveAnywhere();
    void triggerSaveSlot(int slot, bool allowQuickOverwrite = false);
    void triggerLoadSlot(int slot);
    int currentSlot() const { return mSaveSlot; }
    void setCurrentSlot(int s) { mSaveSlot = (s >= 1 && s <= 5) ? s : 1; }
    void showToast(const std::string& msg);
    void setGamepadConnected(bool connected);
    Settings& settings() { return mSettings; }
    // The backend that actually initialised (1 = Vulkan 1.0, 0 = OpenGL ES):
    // OpenGL takes over when no usable Vulkan device or driver is present, so
    // the requested setting and the running backend can differ.
    void setActiveBackend(int backend) { mActiveBackend = backend; }
    int activeBackend() const { return mActiveBackend; }
    // Device name reported by the running backend (set from the Vulkan device
    // properties when Vulkan is active, from GL_RENDERER otherwise).
    void setGpuRendererName(const std::string& name) { if (!name.empty()) mGpuRendererName = name; }
    void reportGpuRenderTime(int64_t renderDurationNs) {
        if (renderDurationNs > 0) {
            mGpuRenderTimeAccumNs += renderDurationNs;
            mGpuRenderFrameCount++;
        }
    }
    void setPlusEdition(bool plus) { mPlusEdition = plus; }
    bool isPlusEdition() const { return mPlusEdition; }

    // Pointer position (Star Pointer)
    void setPointerPos(float x, float y) {
        // Keep the full hand cursor inside the rendered area instead of
        // letting its sprite extend past the screen edge.
        mPointerX = x < -0.90f ? -0.90f : (x > 0.90f ? 0.90f : x);
        mPointerY = y < -0.90f ? -0.90f : (y > 0.90f ? 0.90f : y);
    }
    float getPointerX() const { return mPointerX; }
    float getPointerY() const { return mPointerY; }

    // Touch controls edit mode
    bool isEditModeActive() const { return mEditLayoutMode; }
    void setEditMode(bool active);
    void resetLayoutOffsets();
    void beginControllerCapture(int action);
    bool captureControllerKey(int keyCode, bool down);

    // Save Data Import / Export
    bool exportSave();
    bool importSave();

private:
    UIOverlay();

    // Positions (px) of A, B, Z and SPIN for a given screen size.  Shared by
    // input hit-testing, the overlay drawing and the layout editor so a button
    // always reacts where it is drawn.
    struct ButtonLayout {
        float ax, ay, ar;
        float bx, by, br;
        float zx, zy, zr;
        float spinX, spinY, spinR;
        float pauseX, pauseY, pauseR;
    };
    ButtonLayout buttonLayout(int screenW, int screenH) const;

    // Clear every layout offset: pad clusters and individual buttons.
    void clearLayoutOffsets();
    void refreshLogCache(bool force = false);
    void loadAchievementStats();
    void saveAchievementStats();
    bool achievementsUseSpanish() const;

    Settings mSettings;
    std::string mSavePath;
    std::string mAchievementsPath;
    uint64_t mAchievementTotalPlayedSeconds = 0;
    int64_t mAchievementLastPlayedEpochSeconds = 0;
    int64_t mAchievementSessionStartMs = 0;
    int64_t mAchievementLastPersistMs = 0;
    bool mAchievementFirstGameStarted = false;
    bool mAchievementGameplayActive = false;
    bool mAchievementResumeAfterPause = false;
    bool mAchievementClearConfirm = false;
    bool mGamepadConnected = false;
    int64_t mAchievementToastUntilMs = 0;   // legacy (unused)
    std::string mAchievementToastMessage;   // legacy (unused)

    // Achievements (see achievements.h)
    achv::Tracker mAch;
    int mAchFilter = 0;          // 0 = all, 1 = locked only, 2 = unlocked only
    int mAchResetIndex = -1;     // -1 = reset every achievement, otherwise the one to reset
    int64_t mAchLastEvalMs = 0;
    struct AchToast {
        std::string title, name, desc;
    };
    std::mutex mAchToastLock;
    std::vector<AchToast> mAchToastQueue;
    AchToast mAchToastCurrent;
    bool mAchToastActive = false;
    int64_t mAchToastStartMs = 0;
    static constexpr int64_t kAchToastMs = 10000;
    uint64_t achievementPlaySeconds() const;
    void evaluateAchievements();
    std::vector<int> achievementFilteredList() const;
    float achievementsTabContentHeight() const;
    int mSaveSlot = 1;
    int mLoadSlot = 1;
    int mOverwriteConfirmSlot = 0;
    int64_t mOverwriteConfirmUntilMs = 0;
    int mActiveBackend = 1;  // what actually initialised (see setActiveBackend)
    bool mPlusEdition = true;
    bool mInitialized = false;
    bool mDirty = false;

    // UI state
    bool mSettingsOpen = false;
    float mDrawerAnim = 0.0f; // 0.0 = closed offscreen, 1.0 = fully open
    int64_t mLastAnimTime = 0;
    bool mEditLayoutMode = false;
    bool mDebugEditChecked = false;
    int mDraggingCluster = 0; // 0 = none, 1 = stick cluster, 2 = button cluster
    int mDraggingButton = -1; // A, B, Z, SPIN, or PAUSE
    int mControllerCaptureAction = -1; // A, B, C, Z, SPIN, PAUSE
    float mInitialOwnOfsX = 0.0f, mInitialOwnOfsY = 0.0f;
    float mDragStartX = 0.0f, mDragStartY = 0.0f;
    float mInitialStickOfsX = 0.0f, mInitialStickOfsY = 0.0f;
    float mInitialBtnOfsX = 0.0f, mInitialBtnOfsY = 0.0f;
    float mInitialDpadOfsX = 0.0f, mInitialDpadOfsY = 0.0f;

    // Settings pages: General, Plus mods, saves, controls, achievements.
    int mSettingsTab = 0;              // 0 = General, 1 = Mods, 2 = Saves, 3 = Controls, 4 = Achievements
    float mDrawerScrollY = 0.0f;       // Current scroll offset (pixels)
    float mDrawerScrollTarget = 0.0f;
    bool mDrawerIsScrolling = false;
    int mDrawerScrollPointerId = -1;
    float mDrawerTouchStartY = 0.0f;
    float mDrawerTouchStartX = 0.0f;
    float mDrawerScrollStartOffset = 0.0f;

    // Virtual Stick State
    int mStickPointerId = -1;
    float mStickBaseX = 0.0f;
    float mStickBaseY = 0.0f;
    float mStickKnobX = 0.0f;
    float mStickKnobY = 0.0f;
    float mTouchStickX = 0.0f;
    float mTouchStickY = 0.0f;

    // Camera joystick state
    int mCamStickPointerId = -1;
    float mCamStickKnobX = 0.0f;
    float mCamStickKnobY = 0.0f;
    float mCamStickX = 0.0f;
    float mCamStickY = 0.0f;
    int64_t mCamResetFlashUntilMs = 0;
    float mInitialCamOfsX = 0.0f, mInitialCamOfsY = 0.0f;
    float mInitialResetOfsX = 0.0f, mInitialResetOfsY = 0.0f;

    // Button press states (for visual press effect)
    bool mBtnAPressed = false;
    bool mBtnBPressed = false;
    bool mBtnZPressed = false;
    bool mBtnSpinPressed = false;
    bool mBtnPlusPressed = false;
    bool mBtnCfgPressed = false;
    bool mBtnDpadLeft = false;
    bool mBtnDpadRight = false;
    bool mBtnDpadUp = false;
    bool mBtnDpadDown = false;

    // Persistent in-game Star Pointer state (normalized -1..1, default to screen center 0, 0)
    float mPointerX = 0.0f;
    float mPointerY = 0.0f;

    // Finger active press tracking (prevents stuck buttons)
    bool mFingerDownOnA = false;
    bool mFingerDownOnB = false;
    bool mFingerDownOnZ = false;
    bool mFingerDownOnSpin = false;
    bool mFingerDownOnPlus = false;

    // Multi-touch Pointer Roles
    enum class TouchRole {
        NONE,
        STICK,
        DPAD,
        BTN_A,
        BTN_B,
        BTN_Z,
        BTN_SPIN,
        BTN_PLUS,
        CAM_STICK,
        CAM_RESET,
        STAR_POINTER
    };

    struct TouchPoint {
        int32_t id = -1;
        TouchRole role = TouchRole::NONE;
        float startX = 0.0f;
        float startY = 0.0f;
        float lastX = 0.0f;
        float lastY = 0.0f;
        int64_t startTime = 0;
    };
    TouchPoint mTouches[10];

    // Full Touch Gesture & Tap State
    int mTapPendingFrames = 0;
    int mTapDelayFrames = 0;
    int mTouchPointerId = -1;
    int64_t mTouchStartTime = 0;
    float mTouchStartX = 0.0f, mTouchStartY = 0.0f;
    int64_t mLastTapTime = 0;

    // Hardware & Performance Monitoring Metrics
    float mCurrentFps = 60.0f;
    float mAvgFrameTimeMs = 16.6f;
    int mFpsFrameCounter = 0;
    int mGameFrameCounter = 0;
    float mGameFps = 60.0f;
    int64_t mFpsLastTimeNs = 0;
    float mCpuUsagePercent = 0.0f;
    float mGpuUsagePercent = 0.0f;
    float mRamUsageMB = 0.0f;
    int64_t mGpuRenderTimeAccumNs = 0;
    int mGpuRenderFrameCount = 0;
    int64_t mLastHwSampleTime = 0;
    std::string mGpuRendererName = "Mali-G68 GPU";

    // In-Game Log Viewer State
    bool mLogViewerOpen = false;
    float mLogScrollY = 0.0f;
    float mLogTouchLastY = 0.0f;
    bool mLogTouchScrolling = false;
    int mLogFilter = 0;  // 0 all, 1 saves, 2 loads, 3 graphics, 4 audio, 5 problems
    std::string mLogText;
    struct LogLineRef {
        uint32_t offset;
        uint32_t length;
    };
    std::vector<LogLineRef> mLogFilteredLines;
    int64_t mLogLastRefreshMs = 0;
    int mLogLineCount = 0;
    int mLogErrorCount = 0;
    int mLogWarningCount = 0;

    // Toast Notification State
    std::string mToastMessage = "Game Progress Saved Anywhere!";
    float mSaveToastTimer = 0.0f;

    // GL resources
    unsigned int mProg = 0;
    unsigned int mVao = 0;
    unsigned int mVbo = 0;
    unsigned int mFontTex = 0;
    int mAtlasW = 0;
    int mAtlasH = 0;
};

inline UIOverlay& get() {
    return UIOverlay::get();
}

}  // namespace mobile_ui
