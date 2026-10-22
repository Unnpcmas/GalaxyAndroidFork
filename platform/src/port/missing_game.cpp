// Definitions the original binary had (or never needed) that the decomp does
// not provide as out-of-line code.
#include <string.h>

#include "port/port.h"
#include "Game/Effect/ParticleEmitter.hpp"
#include "Game/System/StationedArchiveLoader.hpp"
#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "nw4r/lyt/pane.h"
#include "nw4r/ut/TextWriterBase.h"

// Base-class draw of the J3D packet hierarchy does nothing.
void J3DPacket::draw() {}

// Only derived conditions are ever instantiated; the base accepts everything.
bool StationedArchiveLoader::Condition::isExecute(const MR::StationedFileInfo*) const { return true; }

namespace nw4r {
    namespace lyt {
        Pane::Pane() {
            Init();
            mBasePosition = 4;  // centre/centre
            memset(mName, 0, sizeof(mName));
            memset(mUserData, 0, sizeof(mUserData));
            mTranslate = math::VEC3(0.0f, 0.0f, 0.0f);
            mRotate = math::VEC3(0.0f, 0.0f, 0.0f);
            mScale = math::VEC2(1.0f, 1.0f);
            mSize = Size(0.0f, 0.0f);
            mAlpha = 0xFF;
            mGlbAlpha = mAlpha;
            mFlag = 0;
            SetVisible(true);
        }
    }  // namespace lyt

    namespace ut {
        // The game only prints wide strings (see CustomTagProcessor.cpp).
        template <>
        f32 TextWriterBase< char >::PrintImpl(StreamType str, int length) {
            (void)str;
            (void)length;
            return 0.0f;
        }
    }  // namespace ut
}  // namespace nw4r

extern "C" {
__attribute__((weak)) int port_is_fullscreen_aspect(void) { return 0; }
__attribute__((weak)) float port_get_display_aspect(void) { return 16.0f / 9.0f; }
__attribute__((weak)) int port_is_fly_mode(void) { return 0; }
__attribute__((weak)) int port_is_force_luigi(void) { return 0; }
__attribute__((weak)) int port_is_skip_cutscenes(void) { return 0; }
__attribute__((weak)) int port_is_infinite_health(void) { return 0; }
__attribute__((weak)) int port_difficulty_enemy_damage(void) { return 1; }
__attribute__((weak)) int port_difficulty_boss_damage(void) { return 1; }
__attribute__((weak)) int port_difficulty_one_hit(void) { return 0; }
__attribute__((weak)) int port_difficulty_no_heal(void) { return 0; }
__attribute__((weak)) int port_difficulty_max_health_3(void) { return 0; }
__attribute__((weak)) int port_rando_active(void) { return 0; }
__attribute__((weak)) const char* port_rando_galaxy(const char* stage) { return stage; }
__attribute__((weak)) int port_rando_scenario(const char*, int selected, int) { return selected; }
__attribute__((weak)) const char* port_rando_object(const char* name) { return name; }
__attribute__((weak)) void port_rando_commit(void) {}
__attribute__((weak)) int port_is_infinite_lives(void) { return 0; }
__attribute__((weak)) int port_is_infinite_starbits(void) { return 0; }
__attribute__((weak)) int port_is_shield_hd_textures(void) { return 1; }
__attribute__((weak)) int port_is_shield_cutscenes(void) { return 1; }
__attribute__((weak)) int port_is_shield_controls(void) { return 1; }
__attribute__((weak)) int port_is_classic_gamepad_ui(void) { return 1; }
__attribute__((weak)) int port_is_riivolution_mods(void) { return 1; }
__attribute__((weak)) int port_input_real_a(void) { return 0; }
__attribute__((weak)) int port_input_real_z(void) { return 0; }

static int sQuickSaveRequested = 0;
static int sQuickSaveSlot = 4;
__attribute__((weak)) void port_trigger_quick_save(int slot) {
    sQuickSaveSlot = slot;
    __atomic_store_n(&sQuickSaveRequested, 1, __ATOMIC_RELEASE);
}
__attribute__((weak)) int port_check_quick_save_request(int* slot) {
    int requested = __atomic_exchange_n(&sQuickSaveRequested, 0, __ATOMIC_ACQ_REL);
    if (slot) *slot = sQuickSaveSlot;
    return requested;
}

static int sQuickLoadRequested = 0;
static int sQuickLoadSlot = 1;
__attribute__((weak)) void port_trigger_quick_load(int slot) {
    sQuickLoadSlot = slot;
    __atomic_store_n(&sQuickLoadRequested, 1, __ATOMIC_RELEASE);
}
__attribute__((weak)) int port_check_quick_load_request(int* slot) {
    if (slot) *slot = sQuickLoadSlot;
    return __atomic_exchange_n(&sQuickLoadRequested, 0, __ATOMIC_ACQ_REL);
}

__attribute__((weak)) int port_nand_save_slot_exists(int slot) { (void)slot; return 0; }
__attribute__((weak)) int port_nand_snapshot_save_slot(int slot, int userFileIndex, int isPlayerMario) {
    (void)slot;
    (void)userFileIndex;
    (void)isPlayerMario;
    return 0;
}
__attribute__((weak)) int port_nand_restore_save_slot(int slot, int* userFileIndex, int* isPlayerMario) {
    (void)slot;
    if (userFileIndex) *userFileIndex = 0;
    if (isPlayerMario) *isPlayerMario = -1;
    return 0;
}
__attribute__((weak)) int port_nand_commit_restore(void) { return 1; }
__attribute__((weak)) int port_nand_rollback_restore(void) { return 1; }
__attribute__((weak)) void port_mobile_save_slot_result(int slot, int success) {
    (void)slot;
    (void)success;
}

__attribute__((weak)) void port_game_quick_save_result(int success) {
    (void)success;
}
__attribute__((weak)) void port_mobile_load_slot_result(int slot, int success) {
    (void)slot;
    (void)success;
}

__attribute__((weak)) void port_android_vibrate(int ms) { (void)ms; }
}
