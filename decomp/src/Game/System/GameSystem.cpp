#include "Game/System/GameSystem.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Screen/HomeButtonLayout.hpp"
#include "Game/Screen/SystemWipeHolder.hpp"
#include "Game/System/AudSystemWrapper.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/FileRipper.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameDataFunction.hpp"
#include "Game/System/GameSystemDimmingWatcher.hpp"
#include "Game/System/GameSystemErrorWatcher.hpp"
#include "Game/System/GameSystemException.hpp"
#include "Game/System/GameSystemFontHolder.hpp"
#include "Game/System/GameSystemFrameControl.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/GameSystemStationedArchiveLoader.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/System/HomeButtonStateNotifier.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "port/port.h"
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <nw4r/lyt/init.h>
#include <revolution.h>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
extern "C" void port_mobile_achievement_set_gameplay_active(int active);
extern "C" void port_mobile_achievement_tick(void);
extern "C" void port_mobile_achievement_poll(int userFile, int stars, int starMax, int grandStars, int starBits,
                                             int coins, int lives);
extern "C" void port_mobile_achievement_stage(const char* stage, int luigi);
extern "C" void port_crash_set_scene(const char* scene, const char* stage);
#endif

#define GX_FIFO_SIZE 0x80000

#define INIT_AUDIO_KEY "\x83\x49\x81\x5b\x83\x66\x83\x42\x83\x49\x8f\x89\x8a\xfa\x89\xbb"  // "Audio Initialization"

namespace NrvGameSystem {
    NEW_NERVE(GameSystemInitializeAudio, GameSystem, InitializeAudio);
    NEW_NERVE(GameSystemInitializeLogoScene, GameSystem, InitializeLogoScene);
    NEW_NERVE(GameSystemLoadStationedArchive, GameSystem, LoadStationedArchive);
    NEW_NERVE(GameSystemWaitForReboot, GameSystem, WaitForReboot);
    NEW_NERVE(GameSystemNormal, GameSystem, Normal);
};  // namespace NrvGameSystem

namespace {
    enum class QuickLoadPhase {
        Idle,
        WaitForSaveSequence,
        WaitForPreload,
        WaitForSceneStart,
    };

    int sQueuedQuickSaveSlot = -1;
    int sRunningQuickSaveSlot = -1;
    bool sQuickSaveSequenceDone = false;
    bool sQuickSaveSequenceSucceeded = false;
    int sQuickLoadSlot = -1;
    int sQuickLoadUserFileIndex = 1;
    bool sQuickLoadPlayerMario = true;
    QuickLoadPhase sQuickLoadPhase = QuickLoadPhase::Idle;
    char sQuickLoadStage[128] = {};
    int64_t sQuickLoadSceneDeadlineNs = 0;
    bool sQuickLoadSawSceneTransition = false;
    // Timings for the save/load log lines.
    bool sAchievementGameplay = false;
    int sAchievementPollFrames = 0;
    int64_t sSaveQueuedNs = 0;
    int64_t sSaveStartedNs = 0;
    int64_t sLoadQueuedNs = 0;
    int64_t sLoadStagedNs = 0;
    inline double msSince(int64_t startNs) {
        return startNs > 0 ? (double)(port_host_time_ns() - startNs) / 1.0e6 : -1.0;
    }
}

extern "C" void port_game_quick_save_result(int success) {
    if (sRunningQuickSaveSlot < 0) {
        port_log("WARN save: received native completion with no waiting slot");
        return;
    }
    sQuickSaveSequenceDone = true;
    sQuickSaveSequenceSucceeded = success != 0;
    port_log(success ? "save: native GameData write completed successfully (%.0f ms after it started)" :
                      "ERROR save: native GameData write failed (%.0f ms after it started)",
             msSince(sSaveStartedNs));
}

#ifdef TARGET_PC
void port_game_main(void) {
#else
#ifdef TARGET_PC
void port_game_main(void) {
#else
void main(void) {
#endif
#endif
    OSInitFastCast();
    DVDInit();
    VIInit();
    HeapMemoryWatcher::createRootHeap();
    OSInitMutex(&MR::MutexHolder< 0 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 1 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 2 >::sMutex);
    nw4r::lyt::LytInit();
    MR::setLayoutDefaultAllocator();
    SingletonHolder< HeapMemoryWatcher >::init();
    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    FileRipper::setup(0x20000, MR::getStationedHeapNapa());
    GameSystemException::init();
    MR::initAcosTable();
    SingletonHolder< GameSystem >::init();
    SingletonHolder< GameSystem >::get()->init();

    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();

    while (true) {
        pGameSystem->frameLoop();
    }
}

GameSystem::GameSystem()
    : NerveExecutor("GameSystem"), mFifoBase(nullptr), mSequenceDirector(nullptr), mErrorWatcher(nullptr), mFontHolder(nullptr),
      mFrameControl(nullptr), mObjHolder(nullptr), mSceneController(nullptr), mStationedArchiveLoader(nullptr), mHomeButtonLayout(nullptr),
      mSystemWipeHolder(nullptr), mHomeButtonStateNotifier(nullptr), mIsExecuteLoadSystemArchive(false) {
}

void GameSystem::init() {
    JKRAram::create(0xE00000, 0xFFFFFFFF, 8, 7, 3);
    mObjHolder = new GameSystemObjHolder();
    mFontHolder = new GameSystemFontHolder();
    mFontHolder->createFontFromEmbeddedData();
    initNerve(GET_NERVE(GameSystem, GameSystemInitializeAudio));
    mSequenceDirector = new GameSequenceDirector();
    initGX();
    DrawSyncManager::start(0x300, 15);
    mSceneController = new GameSystemSceneController();
    mObjHolder->init();
    mErrorWatcher = new GameSystemErrorWatcher();
    mFrameControl = new GameSystemFrameControl();
    SingletonHolder< GameSystemResetAndPowerProcess >::init();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->initWithoutIter();
    mStationedArchiveLoader = new GameSystemStationedArchiveLoader();
    mHomeButtonLayout = new HomeButtonLayout();
    mHomeButtonStateNotifier = new HomeButtonStateNotifier();
    mDimmingWatcher = new GameSystemDimmingWatcher();
    setNerve(GET_NERVE(GameSystem, GameSystemInitializeAudio));
}

bool GameSystem::isExecuteLoadSystemArchive() const {
    return mIsExecuteLoadSystemArchive;
}

bool GameSystem::isDoneLoadSystemArchive() const {
    return isNerve(GET_NERVE(GameSystem, GameSystemNormal));
}

void GameSystem::startToLoadSystemArchive() {
    mIsExecuteLoadSystemArchive = true;

    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    SingletonHolder< NameObjRegister >::get()->setCurrentHolder(mObjHolder->mObjHolder);
    setNerve(GET_NERVE(GameSystem, GameSystemLoadStationedArchive));
}

void GameSystem::exeInitializeAudio() {
    if (MR::isFirstStep(this)) {
        MR::startFunctionAsyncExecute(MR::Functor(mObjHolder, &GameSystemObjHolder::createAudioSystem), 14, INIT_AUDIO_KEY);
    }

    updateSceneController();

    if (MR::isEndFunctionAsyncExecute(INIT_AUDIO_KEY) && mObjHolder->mAudioSystem->isLoadDoneWaveDataAtSystemInit()) {
        MR::waitForEndFunctionAsyncExecute(INIT_AUDIO_KEY);
        setNerve(GET_NERVE(GameSystem, GameSystemInitializeLogoScene));
    }
}

void GameSystem::exeInitializeLogoScene() {
    if (GameSystemFunction::isResetProcessing()) {
        setNerve(GET_NERVE(GameSystem, GameSystemWaitForReboot));
    } else {
        if (MR::isFirstStep(this)) {
            MR::requestChangeScene("Logo");
        }

        updateSceneController();
    }
}

void GameSystem::exeLoadStationedArchive() {
    mStationedArchiveLoader->update();
    updateSceneController();

    if (mStationedArchiveLoader->isDone()) {
        setNerve(GET_NERVE(GameSystem, GameSystemNormal));
    }
}

void GameSystem::exeWaitForReboot() {
}

void GameSystem::exeNormal() {
    updateSceneController();
    mStationedArchiveLoader->update();
}

void GameSystem::initGX() {
    if (mFifoBase == nullptr) {
        mFifoBase = new (32) u8[GX_FIFO_SIZE];
    }

    GXInit(mFifoBase, GX_FIFO_SIZE);
}

void GameSystem::initAfterStationedResourceLoaded() {
    mFontHolder->createFontFromFile();
    mObjHolder->initAfterStationedResourceLoaded();
    mHomeButtonLayout->initWithoutIter();
    mErrorWatcher->initAfterResourceLoaded();
    mSystemWipeHolder = MR::createSystemWipeHolder();
    mSceneController->initAfterStationedResourceLoaded();
    mSequenceDirector->initAfterResourceLoaded();
}

void GameSystem::prepareReset() {
    mStationedArchiveLoader->prepareReset();
}

inline bool isSystemWaitForReboot(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(GET_NERVE(GameSystem, GameSystemWaitForReboot));
}

inline bool isSystemNormal(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(GET_NERVE(GameSystem, GameSystemNormal));
}

bool GameSystem::isPreparedReset() const {
    return isSystemWaitForReboot(this) || isSystemNormal(this) || mStationedArchiveLoader->isPreparedReset();
}

void GameSystem::frameLoop() {
#ifdef TARGET_PC
    port_perf_frame_begin();
#ifndef TARGET_MOBILE
    if (port_skip_fast_forwarding()) {
        // A cutscene being skipped: nothing is drawn (the headset view is
        // dimmed), and the game logic runs as often as fits in a frame.  The
        // retrace wait still gives the other threads (audio, loading) their
        // turn.
        int64_t start = port_host_time_ns();
        do {
            update();
            calcAnim();
            if (mSceneController->isChangingScene()) {
                // The skipped cutscene ends in another stage (Mario jumping
                // out of a dome into the star select): the scene change runs
                // at the normal pace, drawn, with its loading threads timed
                // as when nothing is skipped.  Fast-forwarded through, the
                // game stopped for good there.
                port_skip_scene_change();
                break;
            }
        } while (port_skip_fast_forwarding() && port_host_time_ns() - start < 12000000);
        port_perf_frame_work_done();
        MainLoopFramework::sManager->waitForRetrace();
        return;
    }
#else
    if (port_skip_fast_forwarding()) {
        int64_t start = port_host_time_ns();
        do {
            update();
            calcAnim();
            if (mSceneController->isChangingScene()) {
                port_skip_scene_change();
                break;
            }
        } while (port_skip_fast_forwarding() && port_host_time_ns() - start < 12000000);
    }
#endif
#endif
    MainLoopFramework::sManager->beginRender();
    draw();
    MainLoopFramework::sManager->endRender();

    int requestedSaveSlot = 4;
    if (port_check_quick_save_request(&requestedSaveSlot)) {
        if (requestedSaveSlot < 1 || requestedSaveSlot > 5 ||
            sQueuedQuickSaveSlot >= 0 || sRunningQuickSaveSlot >= 0 ||
            sQuickLoadPhase != QuickLoadPhase::Idle) {
            port_log("ERROR save: rejected overlapping/invalid slot request %d", requestedSaveSlot);
            port_mobile_save_slot_result(requestedSaveSlot, 0);
        } else {
            sQueuedQuickSaveSlot = requestedSaveSlot;
            sSaveQueuedNs = port_host_time_ns();
            const char* queuedStage = MR::getCurrentStageName();
            port_log("save: queued slot %d; waiting for the game save sequence (stage=%s userFile=%d player=%s)",
                     requestedSaveSlot, queuedStage ? queuedStage : "(unknown)", GameDataFunction::getUserFileIndex(),
                     GameDataFunction::isDataMario() ? "Mario" : "Luigi");
        }
    }

    int requestedLoadSlot = 4;
    if (port_check_quick_load_request(&requestedLoadSlot)) {
        if (requestedLoadSlot < 1 || requestedLoadSlot > 5 ||
            sQuickLoadPhase != QuickLoadPhase::Idle) {
            port_log("ERROR load: rejected overlapping/invalid slot request %d", requestedLoadSlot);
            port_mobile_load_slot_result(requestedLoadSlot, 0);
        } else {
            sQuickLoadSlot = requestedLoadSlot;
            const int activeUserFileIndex = GameDataFunction::getUserFileIndex();
            sQuickLoadUserFileIndex =
                activeUserFileIndex >= 1 && activeUserFileIndex <= 6
                    ? activeUserFileIndex
                    : 1;
            sQuickLoadPlayerMario = GameDataFunction::isDataMario();
            const char* stage = MR::getCurrentStageName();
            snprintf(sQuickLoadStage, sizeof(sQuickLoadStage), "%s",
                     stage ? stage : "");
            sQuickLoadSceneDeadlineNs = 0;
            sQuickLoadSawSceneTransition = false;
            sQuickLoadPhase = QuickLoadPhase::WaitForSaveSequence;
            sLoadQueuedNs = port_host_time_ns();
            port_log("load: queued slot %d from stage %s (current userFile=%d player=%s)",
                     sQuickLoadSlot, sQuickLoadStage[0] ? sQuickLoadStage : "(unknown)", sQuickLoadUserFileIndex,
                     sQuickLoadPlayerMario ? "Mario" : "Luigi");
        }
    }

    if (sQueuedQuickSaveSlot >= 0 && sRunningQuickSaveSlot < 0 &&
        !GameSequenceFunction::isActiveSaveDataHandleSequence()) {
        const int slot = sQueuedQuickSaveSlot;
        sQueuedQuickSaveSlot = -1;
        sQuickSaveSequenceDone = false;
        sQuickSaveSequenceSucceeded = false;
        if (GameSequenceFunction::quickSaveAnywhere()) {
            sRunningQuickSaveSlot = slot;
            const char* stage = MR::getCurrentStageName();
            sSaveStartedNs = port_host_time_ns();
            port_log("save: game-data write started for slot %d stage=%s (waited %.0f ms for the game's save sequence)",
                     slot, stage ? stage : "(unknown)", msSince(sSaveQueuedNs));
        } else {
            port_log("ERROR save: game could not start save sequence for slot %d", slot);
            port_mobile_save_slot_result(slot, 0);
        }
    }

    update();
    calcAnim();

#if defined(__ANDROID__)
    if (mSceneController && mSceneController->isFirstUpdateSceneNerveNormal()) {
        const SceneControlInfo& info = mSceneController->mCurrSceneControlInfo;
        const bool gameplayScene =
            std::strcmp(info.mScene, "Game") == 0 &&
            std::strcmp(info.mStage, "FileSelect") != 0 &&
            std::strcmp(info.mStage, "Title") != 0;
        static int64_t sPrevSceneStartNs = 0;
        static int sSceneCount = 0;
        const int64_t nowNs = port_host_time_ns();
        port_log("engine: scene #%d started scene=%s stage=%s gameplay=%d (previous scene lasted %.1f s)",
                 ++sSceneCount, info.mScene, info.mStage, gameplayScene ? 1 : 0,
                 sPrevSceneStartNs > 0 ? (double)(nowNs - sPrevSceneStartNs) / 1.0e9 : 0.0);
        sPrevSceneStartNs = nowNs;
        port_log("achievements: scene started scene=%s stage=%s gameplay=%d",
                 info.mScene, info.mStage, gameplayScene ? 1 : 0);
        port_crash_set_scene(info.mScene, info.mStage);
        port_mobile_achievement_set_gameplay_active(gameplayScene ? 1 : 0);
        sAchievementGameplay = gameplayScene;
        sAchievementPollFrames = 0;
        if (gameplayScene) {
            port_mobile_achievement_stage(info.mStage, GameDataFunction::isDataMario() ? 0 : 1);
        }
    }
    port_mobile_achievement_tick();
    // Facts for the achievements, about once a second while playing.
    if (sAchievementGameplay && ++sAchievementPollFrames >= 60 && MR::isInitializeStateEnd()) {
        sAchievementPollFrames = 0;
        static int sStarMax = 0;
        if (sStarMax <= 0) {
            sStarMax = GameDataFunction::getPowerStarNumMax();
        }
        int grandStars = 0;
        for (int i = 1; i <= 7; i++) {
            if (GameDataFunction::hasGrandStar(i)) {
                grandStars++;
            }
        }
        port_mobile_achievement_poll(GameDataFunction::getUserFileIndex(), GameDataFunction::calcCurrentPowerStarNum(),
                                     sStarMax, grandStars,
                                     GameDataFunction::getStarPieceNum(), MR::getCoinNum(),
                                     GameDataFunction::getPlayerLeft());
    }
#endif

    if (sRunningQuickSaveSlot >= 0 && sQuickSaveSequenceDone) {
        const int slot = sRunningQuickSaveSlot;
        bool success = sQuickSaveSequenceSucceeded;
        const int activeUserFileIndex = GameDataFunction::getUserFileIndex();
        const bool isPlayerMario = GameDataFunction::isDataMario();
        if (success) {
            success = port_nand_snapshot_save_slot(
                          slot, activeUserFileIndex, isPlayerMario ? 1 : 0) != 0;
        }
        port_log(success ? "save: slot %d finished and verified (%.0f ms from the request, %.0f ms after the game's write ended)" :
                           "ERROR save: slot %d failed; active game data was left intact (%.0f ms from the request, %.0f ms after the game's write ended)",
                 slot, msSince(sSaveQueuedNs), msSince(sSaveStartedNs));
        port_mobile_save_slot_result(slot, success ? 1 : 0);
        sRunningQuickSaveSlot = -1;
        sQuickSaveSequenceDone = false;
        sQuickSaveSequenceSucceeded = false;
    }

    if (sQuickLoadPhase == QuickLoadPhase::WaitForSaveSequence &&
        sQueuedQuickSaveSlot < 0 && sRunningQuickSaveSlot < 0 &&
        !GameSequenceFunction::isActiveSaveDataHandleSequence()) {
        int savedUserFileIndex = 0;
        int savedIsPlayerMario = -1;
        if (port_nand_restore_save_slot(sQuickLoadSlot, &savedUserFileIndex,
                                        &savedIsPlayerMario)) {
            if (savedUserFileIndex >= 1 && savedUserFileIndex <= 6 &&
                (savedIsPlayerMario == 0 || savedIsPlayerMario == 1)) {
                sQuickLoadUserFileIndex = savedUserFileIndex;
                sQuickLoadPlayerMario = savedIsPlayerMario != 0;
            } else {
                port_log("WARN load: slot %d is a legacy snapshot; using active profile file=%d player=%s",
                         sQuickLoadSlot, sQuickLoadUserFileIndex,
                         sQuickLoadPlayerMario ? "Mario" : "Luigi");
            }
            GameSequenceFunction::startPreLoadSaveDataSequence();
            sQuickLoadPhase = QuickLoadPhase::WaitForPreload;
            sLoadStagedNs = port_host_time_ns();
            port_log("load: slot %d staged; loading GameData.bin for user file=%d player=%s (%.0f ms after the request)",
                     sQuickLoadSlot, sQuickLoadUserFileIndex,
                     sQuickLoadPlayerMario ? "Mario" : "Luigi", msSince(sLoadQueuedNs));
        } else {
            port_log("ERROR load: could not stage slot %d; current save was not replaced",
                     sQuickLoadSlot);
            port_mobile_load_slot_result(sQuickLoadSlot, 0);
            sQuickLoadSlot = -1;
            sQuickLoadPhase = QuickLoadPhase::Idle;
        }
    }

    if (sQuickLoadPhase == QuickLoadPhase::WaitForPreload &&
        !GameSequenceFunction::isActiveSaveDataHandleSequence()) {
        const int slot = sQuickLoadSlot;
        if (GameSequenceFunction::isSuccessSaveDataHandleSequence()) {
            GameSequenceFunction::startGameDataLoadSequence(
                sQuickLoadUserFileIndex, sQuickLoadPlayerMario);
            if (GameSequenceFunction::isSuccessSaveDataHandleSequence()) {
                // Match FileSelector::exeDemo: enter loaded gameplay rather
                // than recreating the current FileSelect scene.
                MR::requestChangeStageInGameAfterLoadingGameData();
                sQuickLoadSceneDeadlineNs = port_host_time_ns() + 30LL * 1000000000LL;
                sQuickLoadSawSceneTransition = false;
                sQuickLoadPhase = QuickLoadPhase::WaitForSceneStart;
                port_log("load: slot %d GameData validated; transition requested from stage=%s userFile=%d player=%s (%.0f ms after the request)",
                         slot, sQuickLoadStage[0] ? sQuickLoadStage : "(unknown)",
                         sQuickLoadUserFileIndex,
                         sQuickLoadPlayerMario ? "Mario" : "Luigi", msSince(sLoadQueuedNs));
            } else {
                const bool rolledBack = port_nand_rollback_restore() != 0;
                port_log(rolledBack
                             ? "ERROR load: slot %d GameData load sequence failed; previous save restored"
                             : "FATAL load: slot %d GameData load sequence failed and rollback failed",
                         slot);
                port_mobile_load_slot_result(slot, rolledBack ? 0 : -1);
                sQuickLoadSlot = -1;
                sQuickLoadStage[0] = '\0';
                sQuickLoadPhase = QuickLoadPhase::Idle;
            }
        } else {
            const bool rolledBack = port_nand_rollback_restore() != 0;
            port_log(rolledBack
                         ? "ERROR load: slot %d failed validation; previous save restored"
                         : "FATAL load: slot %d failed validation and save rollback failed",
                     slot);
            port_mobile_load_slot_result(slot, rolledBack ? 0 : -1);
            sQuickLoadSlot = -1;
            sQuickLoadStage[0] = '\0';
            sQuickLoadPhase = QuickLoadPhase::Idle;
        }
    }

    if (sQuickLoadPhase == QuickLoadPhase::WaitForSceneStart) {
        const bool sceneStarted = mSceneController &&
                                  mSceneController->isFirstUpdateSceneNerveNormal();
        if (sQuickLoadSceneDeadlineNs > 0 &&
            port_host_time_ns() >= sQuickLoadSceneDeadlineNs) {
            const int slot = sQuickLoadSlot;
            const bool rolledBack = port_nand_rollback_restore() != 0;
            port_log(rolledBack
                         ? "ERROR load: slot %d gameplay scene timed out after 30s; previous save restored"
                         : "FATAL load: slot %d gameplay scene timed out and save rollback failed",
                     slot);
            port_mobile_load_slot_result(slot, rolledBack ? 0 : -1);
            sQuickLoadSlot = -1;
            sQuickLoadStage[0] = '\0';
            sQuickLoadSceneDeadlineNs = 0;
            sQuickLoadSawSceneTransition = false;
            sQuickLoadPhase = QuickLoadPhase::Idle;
        } else if (!sceneStarted) {
            if (!sQuickLoadSawSceneTransition) {
                port_log("load: slot %d scene transition started", sQuickLoadSlot);
            }
            sQuickLoadSawSceneTransition = true;
        } else if (sQuickLoadSawSceneTransition) {
            const int slot = sQuickLoadSlot;
            const char* stage = MR::getCurrentStageName();
            const bool committed = port_nand_commit_restore() != 0;
            port_log(committed
                         ? "load: slot %d loaded; gameplay scene started stage=%s (userFile=%d player=%s, %.0f ms in total)"
                         : "WARN load: slot %d gameplay scene started stage=%s; rollback cleanup remains pending (userFile=%d player=%s, %.0f ms in total)",
                     slot, stage ? stage : "(unknown)", GameDataFunction::getUserFileIndex(),
                     GameDataFunction::isDataMario() ? "Mario" : "Luigi", msSince(sLoadQueuedNs));
            port_mobile_load_slot_result(slot, 1);
            sQuickLoadSlot = -1;
            sQuickLoadStage[0] = '\0';
            sQuickLoadSceneDeadlineNs = 0;
            sQuickLoadSawSceneTransition = false;
            sQuickLoadPhase = QuickLoadPhase::Idle;
        }
    }

    mObjHolder->captureIfAllowForScreenPreserver();
    MainLoopFramework::sManager->endFrame();
#ifdef TARGET_PC
    port_perf_frame_work_done();
#endif
    MainLoopFramework::sManager->waitForRetrace();
}

void GameSystem::draw() {
    mSceneController->drawScene();
#ifdef TARGET_PC
    // Everything drawn over the scene is 2D too: the save and error windows,
    // the pointer's cursor, the system wipe.  In VR it goes on the HUD panel
    // with the scene's own 2D, where the pointer aims (drawn into the eyes'
    // images instead, it covered the whole view, out of the pointer's reach).
    port_gx_marker(PORT_GX_MARK_HUD_BEGIN);
#endif
    mSequenceDirector->draw();
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_POINTER_BEGIN);
#endif
    mObjHolder->drawStarPointer();
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_POINTER_END);
#endif
    mObjHolder->drawBeforeEndRender();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->draw();
    }

    mErrorWatcher->draw();
    mHomeButtonLayout->draw();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->draw();
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_HUD_END);
#endif
}

void GameSystem::update() {
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->movement();
    mSceneController->checkRequestAndChangeScene();
    mObjHolder->update();
    mHomeButtonLayout->movement();

    if (!mHomeButtonLayout->isActive()) {
        mErrorWatcher->movement();
    }

    mDimmingWatcher->_5 = mErrorWatcher->isWarning() || mHomeButtonLayout->isActive() || GameSequenceFunction::isActiveSaveDataHandleSequence();
    mDimmingWatcher->update();
    updateNerve();
}

void GameSystem::updateSceneController() {
    bool isSceneUpdate = true;
    bool isResetProcessing = SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive();

    mObjHolder->updateAudioSystem();

    if (isResetProcessing) {
        isSceneUpdate = false;
    }

    if (mHomeButtonLayout->isActive()) {
        isSceneUpdate = false;
    }

    if (GameSystemFunction::isOccurredSystemWarning()) {
        isSceneUpdate = false;
    }

    mHomeButtonStateNotifier->update(mHomeButtonLayout->isActive() || GameSystemFunction::isOccurredSystemWarning());

    if (isSceneUpdate || isResetProcessing) {
        mSequenceDirector->update();
    }

    if (isSceneUpdate || mSceneController->isFirstUpdateSceneNerveNormal()) {
        if (mSystemWipeHolder != nullptr) {
            mSystemWipeHolder->movement();
        }

        mSceneController->updateScene();
    }

    if (isResetProcessing) {
        mSceneController->updateSceneDuringResetProcessing();
    }
}

void GameSystem::calcAnim() {
    mSceneController->calcAnimScene();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->calcAnim();
    }
}
