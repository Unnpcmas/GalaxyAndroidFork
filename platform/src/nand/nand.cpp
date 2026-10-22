// NAND flash emulation: save data lives in files under the app's storage.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <string>

#include "port/heap_routing.h"
#include "port/port.h"
#include "revolution/nand.h"

static std::string sNandRoot;
static const char kHomeDir[] = "/title/00010000/524d4750/data";
static bool sNandRestoreInProgress = false;
static bool sNandRestoreHadPreviousRoot = false;
static int64_t sNandRestoreStartMs = 0;

static std::string slotDirectory(const std::string& root, int slot) {
    if (slot == 4) return root + "_quick";
    if ((slot >= 1 && slot <= 3) || slot == 5) return root + "_slot" + std::to_string(slot);
    return {};
}

// ---- Save/load diagnostics: sizes, checksums, timings and free space ----------
static int64_t monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// CRC-32 (IEEE) of a whole file; false when it cannot be read.
static bool crc32OfFile(const std::string& path, uint32_t* crcOut, long long* sizeOut) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    long long total = 0;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        total += (long long)n;
    }
    fclose(f);
    *crcOut = ~crc;
    if (sizeOut) *sizeOut = total;
    return true;
}

// "size=48640 bytes, modified 2026-10-21 15:19:55, crc32=1a2b3c4d" or "missing".
static std::string describeFile(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return std::string("missing (") + strerror(errno) + ")";
    char when[32] = "?";
    struct tm tmv;
    time_t mt = st.st_mtime;
    if (localtime_r(&mt, &tmv)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    uint32_t crc = 0;
    long long size = 0;
    char out[160];
    if (crc32OfFile(path, &crc, &size)) {
        snprintf(out, sizeof(out), "size=%lld bytes, modified %s, crc32=%08x", (long long)st.st_size, when, crc);
    } else {
        snprintf(out, sizeof(out), "size=%lld bytes, modified %s, unreadable (%s)", (long long)st.st_size, when, strerror(errno));
    }
    return out;
}

static bool sameFileContent(const std::string& a, const std::string& b) {
    uint32_t ca = 0, cb = 0;
    long long sa = 0, sb = 0;
    return crc32OfFile(a, &ca, &sa) && crc32OfFile(b, &cb, &sb) && ca == cb && sa == sb;
}

static void logFreeSpace(const char* what, const std::string& path) {
    struct statvfs vfs;
    if (statvfs(path.c_str(), &vfs) != 0) {
        port_log("nand: free space for %s unknown (%s)", what, strerror(errno));
        return;
    }
    const long long freeMb = (long long)vfs.f_bavail * (long long)vfs.f_frsize / (1024 * 1024);
    if (freeMb < 100) {
        port_log("WARN nand: only %lld MB free for %s; a save could fail", freeMb, what);
    } else {
        port_log("nand: %lld MB free for %s", freeMb, what);
    }
}

static constexpr char kSlotProfileFile[] = ".galaxy_android_slot_profile";
static constexpr uint32_t kSlotProfileMagic = 0x47534c54;  // "GSLT"
static bool syncDirectory(const std::string& path);

struct SlotProfileMetadata {
    uint32_t magic;
    uint32_t version;
    uint32_t userFileIndex;
    uint32_t isPlayerMario;
    uint32_t checksum;
};

static uint32_t slotProfileChecksum(const SlotProfileMetadata& metadata) {
    return metadata.magic ^ metadata.version ^ metadata.userFileIndex ^
           metadata.isPlayerMario ^ 0xa57c91e3u;
}

static bool writeSlotProfileMetadata(const std::string& root, int userFileIndex,
                                     bool isPlayerMario) {
    SlotProfileMetadata metadata{
        kSlotProfileMagic, 1u, (uint32_t)userFileIndex,
        isPlayerMario ? 1u : 0u, 0u
    };
    metadata.checksum = slotProfileChecksum(metadata);

    const std::string path = root + "/" + kSlotProfileFile;
    const std::string temporary = path + ".tmp";
    int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return false;

    const char* data = reinterpret_cast<const char*>(&metadata);
    size_t remaining = sizeof(metadata);
    bool ok = true;
    while (remaining > 0) {
        ssize_t written = write(fd, data, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        data += written;
        remaining -= (size_t)written;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temporary.c_str(), path.c_str()) != 0) ok = false;
    if (ok) ok = syncDirectory(root);
    if (!ok) unlink(temporary.c_str());
    return ok;
}

static bool readSlotProfileMetadata(const std::string& root, int* userFileIndex,
                                    int* isPlayerMario) {
    if (!userFileIndex || !isPlayerMario) return false;

    const std::string path = root + "/" + kSlotProfileFile;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    SlotProfileMetadata metadata{};
    struct stat metadataStat {};
    if (fstat(fd, &metadataStat) != 0 ||
        metadataStat.st_size != (off_t)sizeof(metadata)) {
        close(fd);
        return false;
    }
    char* data = reinterpret_cast<char*>(&metadata);
    size_t remaining = sizeof(metadata);
    bool ok = true;
    while (remaining > 0) {
        ssize_t count = read(fd, data, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        data += count;
        remaining -= (size_t)count;
    }
    if (close(fd) != 0) ok = false;
    if (!ok || metadata.magic != kSlotProfileMagic || metadata.version != 1u ||
        metadata.userFileIndex < 1u || metadata.userFileIndex > 6u ||
        metadata.isPlayerMario > 1u ||
        metadata.checksum != slotProfileChecksum(metadata)) {
        return false;
    }

    *userFileIndex = (int)metadata.userFileIndex;
    *isPlayerMario = (int)metadata.isPlayerMario;
    return true;
}

static std::string publicSlotDirectory(int slot) {
    if (slot == 4) return "/sdcard/GalaxyData/saves/quick";
    if ((slot >= 1 && slot <= 3) || slot == 5) return "/sdcard/GalaxyData/saves/slot" + std::to_string(slot);
    return {};
}

static bool pathExists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

static bool removeTreeChecked(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return false;
        bool ok = true;
        for (;;) {
            errno = 0;
            struct dirent* entry = readdir(dir);
            if (!entry) {
                if (errno != 0) ok = false;
                break;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (!removeTreeChecked(path + "/" + entry->d_name)) ok = false;
        }
        closedir(dir);
        return ok && rmdir(path.c_str()) == 0;
    }
    return unlink(path.c_str()) == 0;
}

static bool syncDirectory(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static bool copyFileChecked(const std::string& src, const std::string& dst, mode_t mode) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(dst.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, mode & 0770);
    if (out < 0) {
        close(in);
        return false;
    }
    bool ok = true;
    char buffer[16384];
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out, buffer + written, (size_t)(n - written));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok) break;
    }
    if (ok && fsync(out) != 0) ok = false;
    if (close(in) != 0) ok = false;
    if (close(out) != 0) ok = false;
    return ok;
}

static bool copyTreeChecked(const std::string& src, const std::string& dst) {
    struct stat st;
    if (lstat(src.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (mkdir(dst.c_str(), (st.st_mode & 0770) ? (st.st_mode & 0770) : 0770) != 0) return false;
    DIR* dir = opendir(src.c_str());
    if (!dir) return false;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            if (errno != 0) ok = false;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string from = src + "/" + entry->d_name;
        std::string to = dst + "/" + entry->d_name;
        struct stat child;
        if (lstat(from.c_str(), &child) != 0) {
            ok = false;
        } else if (S_ISDIR(child.st_mode)) {
            ok = copyTreeChecked(from, to);
        } else if (S_ISREG(child.st_mode)) {
            ok = copyFileChecked(from, to, child.st_mode);
        } else {
            errno = EINVAL;
            ok = false;
        }
        if (!ok) break;
    }
    closedir(dir);
    return ok && syncDirectory(dst);
}

static bool hasValidGameData(const std::string& root) {
    struct stat st;
    std::string save = root + kHomeDir + "/GameData.bin";
    return lstat(save.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_size >= 0x140 && st.st_size <= 0x10000;
}

static bool atomicCopySaveRoot(const std::string& src, const std::string& dst,
                               int userFileIndex, bool isPlayerMario) {
    const std::string stage = dst + ".tmp";
    const std::string previous = dst + ".previous";
    if (!removeTreeChecked(stage)) return false;
    if (!copyTreeChecked(src, stage) ||
        !writeSlotProfileMetadata(stage, userFileIndex, isPlayerMario) ||
        !hasValidGameData(stage)) {
        removeTreeChecked(stage);
        return false;
    }

    const bool hadPreviousTarget = pathExists(dst);
    if (hadPreviousTarget) {
        if (!removeTreeChecked(previous) || rename(dst.c_str(), previous.c_str()) != 0) {
            removeTreeChecked(stage);
            return false;
        }
    }
    if (rename(stage.c_str(), dst.c_str()) != 0) {
        if (hadPreviousTarget) rename(previous.c_str(), dst.c_str());
        removeTreeChecked(stage);
        return false;
    }

    const size_t slash = dst.find_last_of('/');
    if (slash != std::string::npos) syncDirectory(dst.substr(0, slash));
    if (hadPreviousTarget && !removeTreeChecked(previous)) {
        port_log("nand: warning: could not remove old save-slot snapshot %s", previous.c_str());
    }
    return true;
}

static void recoverSaveDirectory(const std::string& slot) {
    const std::string previous = slot + ".previous";
    const std::string stage = slot + ".tmp";
    if (!hasValidGameData(slot) && hasValidGameData(previous)) {
        removeTreeChecked(slot);
        if (rename(previous.c_str(), slot.c_str()) == 0) {
            port_log("nand: recovered interrupted save-slot update %s", slot.c_str());
        }
    } else {
        removeTreeChecked(previous);
    }
    removeTreeChecked(stage);
}

static void recoverInterruptedRestore() {
    if (sNandRoot.empty()) return;
    const std::string previous = sNandRoot + "_restore_previous";
    const std::string stage = sNandRoot + "_restore_stage";
    const std::string committed = sNandRoot + "_restore_committed";
    if (pathExists(previous)) {
        removeTreeChecked(sNandRoot);
        if (rename(previous.c_str(), sNandRoot.c_str()) == 0) {
            port_log("nand: recovered previous save after interrupted slot load");
        } else {
            port_log("ERROR nand: could not restore rollback save directory %s", previous.c_str());
        }
    }
    removeTreeChecked(stage);
    removeTreeChecked(committed);
    sNandRestoreInProgress = false;
    sNandRestoreHadPreviousRoot = false;
}

// One block in the log with every save the app knows: the active game data and
// the five slots, internal and external, with size, date and checksum.
static void logSaveInventory(const char* why) {
    port_log("save inventory (%s): root=%s", why, sNandRoot.c_str());
    port_log("save inventory:   active   GameData.bin %s", describeFile(sNandRoot + kHomeDir + "/GameData.bin").c_str());
    for (int slot = 1; slot <= 5; ++slot) {
        const std::string internalSlot = slotDirectory(sNandRoot, slot);
        const std::string publicSlot = publicSlotDirectory(slot);
        int user = 0, mario = -1;
        const bool hasMeta = hasValidGameData(internalSlot) && readSlotProfileMetadata(internalSlot, &user, &mario);
        port_log("save inventory:   slot %d %s: internal %s | external %s", slot,
                 hasMeta ? (std::string("(user file ") + std::to_string(user) + ", " + (mario ? "Mario" : "Luigi") + ")").c_str() : "(no profile info)",
                 hasValidGameData(internalSlot) ? describeFile(internalSlot + kHomeDir + "/GameData.bin").c_str() : "none",
                 hasValidGameData(publicSlot) ? describeFile(publicSlot + kHomeDir + "/GameData.bin").c_str() : "none");
    }
    const std::string ext = "/sdcard/GalaxyData/saves/GameData.bin";
    port_log("save inventory:   external mirror GameData.bin %s", describeFile(ext).c_str());
    logFreeSpace("saves (internal storage)", sNandRoot.empty() ? "/data" : sNandRoot);
}

extern "C" void port_nand_set_root(const char* root) {
    PortHostAllocScope scope;
    sNandRoot = root ? root : "";
    recoverInterruptedRestore();
    for (int slot = 1; slot <= 5; ++slot) {
        recoverSaveDirectory(slotDirectory(sNandRoot, slot));
    }
    logSaveInventory("at startup");
}

static std::string hostPath(const char* path) {
    PortHostAllocScope scope;
    std::string p = path ? path : "";
    if (p.empty() || p[0] != '/') {
        p = std::string(kHomeDir) + "/" + p;
    }
    return sNandRoot + p;
}

static void makeDirs(const std::string& path) {
    PortHostAllocScope scope;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            std::string d = path.substr(0, i);
            mkdir(d.c_str(), 0770);
        }
    }
}

static s32 errnoToNand(int e) {
    switch (e) {
    case ENOENT:
        return NAND_RESULT_NOEXISTS;
    case EEXIST:
        return NAND_RESULT_EXISTS;
    case EACCES:
    case EPERM:
        return NAND_RESULT_ACCESS;
    case ENOSPC:
        return NAND_RESULT_MAXBLOCKS;
    case ENOTEMPTY:
        return NAND_RESULT_NOTEMPTY;
    default:
        return NAND_RESULT_UNKNOWN;
    }
}

extern "C" {

int port_nand_save_slot_exists(int slot) {
    PortHostAllocScope scope;
    if (sNandRoot.empty() || slot < 1 || slot > 5) return 0;
    if (hasValidGameData(slotDirectory(sNandRoot, slot))) return 1;
    return hasValidGameData(publicSlotDirectory(slot)) ? 1 : 0;
}

int port_nand_snapshot_save_slot(int slot, int userFileIndex, int isPlayerMario) {
    PortHostAllocScope scope;
    if (sNandRoot.empty() || slot < 1 || slot > 5) return 0;
    if (userFileIndex < 1 || userFileIndex > 6) {
        port_log("ERROR nand: refused slot %d profile metadata with invalid user file %d",
                 slot, userFileIndex);
        return 0;
    }
    if (isPlayerMario != 0 && isPlayerMario != 1) return 0;
    if (!hasValidGameData(sNandRoot)) {
        port_log("ERROR nand: refused slot %d snapshot; no valid GameData.bin in active NAND", slot);
        return 0;
    }

    const int64_t snapStartMs = monoMs();
    struct stat gameDataStat {};
    const std::string activeGameData = sNandRoot + kHomeDir + "/GameData.bin";
    if (stat(activeGameData.c_str(), &gameDataStat) == 0) {
        port_log("save: slot %d active GameData.bin size=%lld bytes",
                 slot, (long long)gameDataStat.st_size);
    }
    port_log("save: snapshot of slot %d started (user file %d, player=%s); active GameData.bin %s",
             slot, userFileIndex, isPlayerMario ? "Mario" : "Luigi", describeFile(activeGameData).c_str());
    logFreeSpace("the slot copy", sNandRoot);
    const std::string internalSlot = slotDirectory(sNandRoot, slot);
    if (!atomicCopySaveRoot(sNandRoot, internalSlot, userFileIndex,
                            isPlayerMario != 0)) {
        port_log("ERROR nand: could not commit slot %d snapshot: %s (after %lld ms)", slot, strerror(errno),
                 (long long)(monoMs() - snapStartMs));
        return 0;
    }
    port_log("nand: committed verified slot %d snapshot at %s (user file %d, player=%s)",
             slot, internalSlot.c_str(), userFileIndex,
             isPlayerMario ? "Mario" : "Luigi");
    {
        const std::string copy = internalSlot + kHomeDir + "/GameData.bin";
        port_log("save: slot %d internal copy %s; identical to the active save: %s; took %lld ms",
                 slot, describeFile(copy).c_str(), sameFileContent(activeGameData, copy) ? "yes" : "NO (mismatch)",
                 (long long)(monoMs() - snapStartMs));
    }

    if (mkdir("/sdcard/GalaxyData", 0775) != 0 && errno != EEXIST) {
        port_log("WARN nand: external slot mirror unavailable: %s", strerror(errno));
        return 1;
    }
    if (mkdir("/sdcard/GalaxyData/saves", 0775) != 0 && errno != EEXIST) {
        port_log("WARN nand: external slot mirror unavailable: %s", strerror(errno));
        return 1;
    }
    const std::string publicSlot = publicSlotDirectory(slot);
    if (!atomicCopySaveRoot(sNandRoot, publicSlot, userFileIndex,
                            isPlayerMario != 0)) {
        port_log("WARN nand: internal slot %d is safe; external mirror failed: %s",
                 slot, strerror(errno));
    } else {
        port_log("save: slot %d external copy %s; whole save took %lld ms",
                 slot, describeFile(publicSlot + kHomeDir + "/GameData.bin").c_str(),
                 (long long)(monoMs() - snapStartMs));
    }
    return 1;
}

int port_nand_restore_save_slot(int slot, int* userFileIndex, int* isPlayerMario) {
    PortHostAllocScope scope;
    if (sNandRoot.empty() || slot < 1 || slot > 5 || sNandRestoreInProgress) return 0;

    const std::string internalSlot = slotDirectory(sNandRoot, slot);
    const std::string publicSlot = publicSlotDirectory(slot);
    const std::string source = hasValidGameData(internalSlot) ? internalSlot : publicSlot;
    if (!hasValidGameData(source)) {
        port_log("ERROR nand: slot %d has no valid GameData.bin", slot);
        return 0;
    }
    int savedUserFileIndex = 0;
    int savedIsPlayerMario = -1;
    const bool hasProfileMetadata =
        readSlotProfileMetadata(source, &savedUserFileIndex, &savedIsPlayerMario);
    struct stat gameDataStat {};
    const std::string sourceGameData = source + kHomeDir + "/GameData.bin";
    sNandRestoreStartMs = monoMs();
    if (stat(sourceGameData.c_str(), &gameDataStat) == 0) {
        port_log("load: slot %d source=%s GameData.bin size=%lld bytes",
                 slot, source == internalSlot ? "internal" : "external",
                 (long long)gameDataStat.st_size);
    }
    port_log("load: slot %d source file %s; current active save %s", slot, describeFile(sourceGameData).c_str(),
             describeFile(sNandRoot + kHomeDir + "/GameData.bin").c_str());

    const std::string stage = sNandRoot + "_restore_stage";
    const std::string previous = sNandRoot + "_restore_previous";
    const std::string committed = sNandRoot + "_restore_committed";
    if (!removeTreeChecked(committed)) {
        port_log("ERROR nand: could not clear previous committed-restore cleanup");
        return 0;
    }
    bool staged = removeTreeChecked(stage) && !pathExists(previous) &&
                  copyTreeChecked(source, stage);
    if (staged) {
        // Slot-only launcher metadata must never be copied into the game's NAND.
        staged = removeTreeChecked(stage + "/" + kSlotProfileFile) &&
                 removeTreeChecked(stage + "/" + kSlotProfileFile + ".tmp") &&
                 hasValidGameData(stage);
    }
    if (!staged) {
        removeTreeChecked(stage);
        port_log("ERROR nand: could not stage slot %d for safe restore: %s", slot, strerror(errno));
        return 0;
    }

    sNandRestoreHadPreviousRoot = pathExists(sNandRoot);
    if (sNandRestoreHadPreviousRoot && rename(sNandRoot.c_str(), previous.c_str()) != 0) {
        removeTreeChecked(stage);
        port_log("ERROR nand: could not protect current save before restore: %s", strerror(errno));
        return 0;
    }
    if (rename(stage.c_str(), sNandRoot.c_str()) != 0) {
        if (sNandRestoreHadPreviousRoot) rename(previous.c_str(), sNandRoot.c_str());
        removeTreeChecked(stage);
        port_log("ERROR nand: could not activate staged slot %d: %s", slot, strerror(errno));
        sNandRestoreHadPreviousRoot = false;
        return 0;
    }
    const size_t slash = sNandRoot.find_last_of('/');
    if (slash != std::string::npos) syncDirectory(sNandRoot.substr(0, slash));
    sNandRestoreInProgress = true;
    if (userFileIndex) *userFileIndex = hasProfileMetadata ? savedUserFileIndex : 0;
    if (isPlayerMario) *isPlayerMario = hasProfileMetadata ? savedIsPlayerMario : -1;
    if (!hasProfileMetadata) {
        port_log("WARN nand: slot %d has no valid active-profile metadata; caller must use a safe legacy profile",
                 slot);
    } else {
        port_log("nand: slot %d profile metadata loaded (user file %d, player=%s)",
                 slot, savedUserFileIndex, savedIsPlayerMario ? "Mario" : "Luigi");
    }
    port_log("nand: slot %d staged safely; waiting for game-data verification", slot);
    port_log("load: slot %d is now the active save: %s; identical to the slot file: %s; staging took %lld ms",
             slot, describeFile(sNandRoot + kHomeDir + "/GameData.bin").c_str(),
             sameFileContent(sNandRoot + kHomeDir + "/GameData.bin", sourceGameData) ? "yes" : "NO (mismatch)",
             (long long)(monoMs() - sNandRestoreStartMs));
    return 1;
}

int port_nand_commit_restore(void) {
    PortHostAllocScope scope;
    if (!sNandRestoreInProgress) return 1;
    const std::string previous = sNandRoot + "_restore_previous";
    const std::string committed = sNandRoot + "_restore_committed";
    if (sNandRestoreHadPreviousRoot) {
        if (!removeTreeChecked(committed) || rename(previous.c_str(), committed.c_str()) != 0) {
            port_log("WARN nand: loaded slot is active but rollback copy remains at %s",
                     previous.c_str());
            return 0;
        }
    }
    sNandRestoreInProgress = false;
    sNandRestoreHadPreviousRoot = false;
    if (!removeTreeChecked(committed)) {
        port_log("WARN nand: committed load is safe; retained cleanup copy at %s",
                 committed.c_str());
    }
    port_log("nand: slot restore committed (%lld ms after the restore started)",
             (long long)(monoMs() - sNandRestoreStartMs));
    logSaveInventory("after a slot load");
    return 1;
}

int port_nand_rollback_restore(void) {
    PortHostAllocScope scope;
    if (!sNandRestoreInProgress) return 1;
    const std::string previous = sNandRoot + "_restore_previous";
    if (!removeTreeChecked(sNandRoot)) {
        port_log("ERROR nand: could not remove failed restored save: %s", strerror(errno));
        return 0;
    }
    if (sNandRestoreHadPreviousRoot && rename(previous.c_str(), sNandRoot.c_str()) != 0) {
        port_log("ERROR nand: could not roll back to previous save: %s", strerror(errno));
        return 0;
    }
    sNandRestoreInProgress = false;
    sNandRestoreHadPreviousRoot = false;
    port_log("nand: failed slot restore rolled back to previous save (%lld ms after the restore started); active save %s",
             (long long)(monoMs() - sNandRestoreStartMs),
             describeFile(sNandRoot + kHomeDir + "/GameData.bin").c_str());
    return 1;
}

s32 NANDInit(void) { return NAND_RESULT_OK; }

s32 NANDGetHomeDir(char path[NAND_MAX_PATH]) {
    strcpy(path, kHomeDir);
    return NAND_RESULT_OK;
}

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
    (void)perm;
    (void)attr;
    std::string hp = hostPath(path);
    makeDirs(hp);
    int fd = open(hp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0660);
    if (fd < 0) {
        return errnoToNand(errno);
    }
    close(fd);
    return NAND_RESULT_OK;
}

s32 NANDPrivateCreate(const char* path, u8 perm, u8 attr) { return NANDCreate(path, perm, attr); }

static int sGameDataFd = -1;
static long long sGameDataRead = 0, sGameDataWritten = 0;
static int64_t sGameDataOpenMs = 0;

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 mode) {
    std::string hp = hostPath(path);
    int flags = O_CLOEXEC;
    switch (mode) {
    case NAND_ACCESS_READ:
        flags |= O_RDONLY;
        break;
    case NAND_ACCESS_WRITE:
        flags |= O_WRONLY;
        break;
    default:
        flags |= O_RDWR;
        break;
    }
    int fd = open(hp.c_str(), flags);
    if (fd < 0) {
        const int err = errno;
        if (strstr(path, "GameData.bin")) {
            port_log("%s nand: could not open GameData.bin (%s): %s", err == ENOENT ? "WARN" : "ERROR", hp.c_str(), strerror(err));
        }
        return errnoToNand(err);
    }
    memset(info, 0, sizeof(*info));
    info->fileDescriptor = fd;
    info->accType = mode;
    strncpy(info->origPath, path, NAND_MAX_PATH - 1);
    if (strstr(path, "GameData.bin")) {
        sGameDataFd = fd;
        sGameDataRead = sGameDataWritten = 0;
        sGameDataOpenMs = monoMs();
        port_log("nand: GameData.bin opened by the game for %s (%s)",
                 mode == NAND_ACCESS_READ ? "reading" : mode == NAND_ACCESS_WRITE ? "writing" : "reading and writing",
                 describeFile(hp).c_str());
    }
    return NAND_RESULT_OK;
}

s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 mode) { return NANDOpen(path, info, mode); }

s32 NANDClose(NANDFileInfo* info) {
    if (info->fileDescriptor >= 0) {
        if (info->fileDescriptor == sGameDataFd && strstr(info->origPath, "GameData.bin")) {
            port_log("nand: GameData.bin closed after %lld bytes read and %lld bytes written (open for %lld ms)",
                     sGameDataRead, sGameDataWritten, (long long)(monoMs() - sGameDataOpenMs));
            sGameDataFd = -1;
        }
        if (info->accType != NAND_ACCESS_READ) {
            const int syncResult = fsync(info->fileDescriptor);
            if (syncResult != 0 && strstr(info->origPath, "GameData.bin")) {
                port_log("ERROR nand: fsync of GameData.bin failed: %s", strerror(errno));
            }
            // Auto-mirror GameData.bin to persistent external storage (/sdcard/GalaxyData/saves)
            if (strstr(info->origPath, "GameData.bin")) {
                std::string hp = hostPath(info->origPath);
                FILE* in = fopen(hp.c_str(), "rb");
                if (in) {
                    const int64_t mirrorStartMs = monoMs();
                    mkdir("/sdcard/GalaxyData", 0770);
                    mkdir("/sdcard/GalaxyData/saves", 0770);
                    FILE* out = fopen("/sdcard/GalaxyData/saves/GameData.bin", "wb");
                    if (out) {
                        char buf[4096];
                        size_t n;
                        long long copied = 0;
                        bool writeOk = true;
                        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                            if (fwrite(buf, 1, n, out) != n) { writeOk = false; break; }
                            copied += (long long)n;
                        }
                        fflush(out);
                        fclose(out);
                        port_log("nand: mirrored %s -> /sdcard/GalaxyData/saves/GameData.bin", hp.c_str());
                        port_log("nand: mirror %s: %lld bytes in %lld ms; external copy %s",
                                 writeOk ? "ok" : "INCOMPLETE (write error)", copied,
                                 (long long)(monoMs() - mirrorStartMs),
                                 describeFile("/sdcard/GalaxyData/saves/GameData.bin").c_str());
                    } else {
                        port_log("WARN nand: could not open the external mirror for writing: %s", strerror(errno));
                    }
                    fclose(in);
                }
            }
        }
        close(info->fileDescriptor);
        info->fileDescriptor = -1;
    }
    return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
    ssize_t n = read(info->fileDescriptor, buf, length);
    if (n > 0 && info->fileDescriptor == sGameDataFd) sGameDataRead += n;
    return n < 0 ? errnoToNand(errno) : (s32)n;
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
    ssize_t n = write(info->fileDescriptor, buf, length);
    if (n > 0 && info->fileDescriptor == sGameDataFd) sGameDataWritten += n;
    if (n < 0 && info->fileDescriptor == sGameDataFd) port_log("ERROR nand: write to GameData.bin failed: %s", strerror(errno));
    return n < 0 ? errnoToNand(errno) : (s32)n;
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
    off_t r = lseek(info->fileDescriptor, offset, whence);
    return r < 0 ? errnoToNand(errno) : (s32)r;
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    struct stat st;
    if (fstat(info->fileDescriptor, &st) != 0) {
        return errnoToNand(errno);
    }
    *length = (u32)st.st_size;
    return NAND_RESULT_OK;
}

static int removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (d) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                    continue;
                }
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        return rmdir(path.c_str());
    }
    return unlink(path.c_str());
}

s32 NANDDelete(const char* path) {
    PortHostAllocScope scope;
    std::string hp = hostPath(path);
    if (removeTree(hp) != 0) {
        return errnoToNand(errno);
    }
    return NAND_RESULT_OK;
}

s32 NANDMove(const char* path, const char* destDir) {
    PortHostAllocScope scope;
    std::string src = hostPath(path);
    std::string name = src.substr(src.rfind('/') + 1);
    std::string dst = hostPath(destDir) + "/" + name;
    makeDirs(dst);
    if (rename(src.c_str(), dst.c_str()) != 0) {
        return errnoToNand(errno);
    }
    return NAND_RESULT_OK;
}

s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer) {
    (void)fsBlock;
    (void)inode;
    *answer = 0;  // enough space and inodes
    return NAND_RESULT_OK;
}

void NANDInitBanner(NANDBanner* banner, u32 flag, const u16* title, const u16* comment) {
    memset(banner, 0, sizeof(*banner));
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flag;
    for (int i = 0; title && i < NAND_BANNER_COMMENT_SIZE && title[i]; i++) {
        banner->comment[0][i] = title[i];
    }
    for (int i = 0; comment && i < NAND_BANNER_COMMENT_SIZE && comment[i]; i++) {
        banner->comment[1][i] = comment[i];
    }
}

}  // extern "C"
