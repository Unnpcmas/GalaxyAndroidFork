#include "dvd/mod_manager.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "port/port.h"

namespace port_mods {
namespace {

constexpr size_t kMaxMods = 48;
constexpr int kMaxDepth = 10;
constexpr int kMaxFilesPerMod = 4096;
constexpr size_t kMaxManifestBytes = 8192;

std::string sModsDirectory;
std::mutex sModsMutex;
std::shared_ptr<const std::vector<ModInfo>> sMods =
    std::make_shared<const std::vector<ModInfo>>();
std::atomic<bool> sGlobalEnabled{true};

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 48 || id == "." || id == "..") return false;
    for (unsigned char c : id) {
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

bool isPlainDirectory(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool isPlainFile(const std::string& path, bool allowEmpty = false) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
           !S_ISLNK(st.st_mode) && (allowEmpty || st.st_size > 0);
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string safeDisplayText(const std::string& input, size_t maxLength) {
    std::string out;
    out.reserve(std::min(input.size(), maxLength));
    for (unsigned char c : input) {
        if (c < 0x20 || c == 0x7f) continue;
        out.push_back((char)c);
        if (out.size() >= maxLength) break;
    }
    return out.empty() ? std::string("Mod sin nombre") : out;
}

bool validRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path[0] == '/' || path[0] == '\\') return false;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) {
            if (c == '\\' || c < 0x20 || c == 0x7f) return false;
        }
        start = end + 1;
    }
    return true;
}

bool parseManifest(const std::string& path, ModInfo& info) {
    if (!isPlainFile(path, true)) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[512];
    size_t total = 0;
    bool sawName = false;
    while (fgets(line, sizeof(line), f)) {
        total += strlen(line);
        if (total > kMaxManifestBytes) {
            fclose(f);
            return false;
        }
        std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;
        const size_t equal = text.find('=');
        if (equal == std::string::npos) continue;
        std::string key = trim(text.substr(0, equal));
        std::string value = trim(text.substr(equal + 1));
        if (key == "name") {
            info.name = safeDisplayText(value, 48);
            sawName = true;
        } else if (key == "version") {
            info.version = safeDisplayText(value, 20);
        } else if (key == "enabled") {
            info.enabled = value == "1" || value == "true" || value == "on";
        } else if (key == "priority") {
            char* end = nullptr;
            long parsed = strtol(value.c_str(), &end, 10);
            if (end && *end == '\0') info.priority = (int)std::max(-100L, std::min(100L, parsed));
        }
    }
    const bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
    if (!sawName) info.name = info.id;
    return true;
}

int countFilesRecursive(const std::string& dir, int depth, int& count) {
    if (depth > kMaxDepth || count >= kMaxFilesPerMod || !isPlainDirectory(dir)) return count;
    DIR* handle = opendir(dir.c_str());
    if (!handle) return count;
    while (dirent* entry = readdir(handle)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) continue;
        const std::string path = dir + "/" + entry->d_name;
        struct stat st {};
        if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            countFilesRecursive(path, depth + 1, count);
        } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
            ++count;
            if (count >= kMaxFilesPerMod) break;
        }
    }
    closedir(handle);
    return count;
}

bool pathContainsNoSymlink(const std::string& base, const std::string& relative,
                           bool finalMustBeFile) {
    if (!validRelativePath(relative) || !isPlainDirectory(base)) return false;
    std::string current = base;
    size_t start = 0;
    while (start < relative.size()) {
        size_t end = relative.find('/', start);
        const bool last = end == std::string::npos;
        if (last) end = relative.size();
        current += "/" + relative.substr(start, end - start);
        struct stat st {};
        if (lstat(current.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) return false;
        if (last) return finalMustBeFile ? S_ISREG(st.st_mode) && st.st_size > 0
                                         : S_ISDIR(st.st_mode);
        if (!S_ISDIR(st.st_mode)) return false;
        start = end + 1;
    }
    return false;
}

bool rewriteEnabled(const std::string& manifest, bool enabled) {
    if (!isPlainFile(manifest, true)) return false;
    FILE* input = fopen(manifest.c_str(), "rb");
    if (!input) return false;
    const std::string temp = manifest + ".tmp";
    struct stat tempStat {};
    if (lstat(temp.c_str(), &tempStat) == 0) {
        if (!S_ISREG(tempStat.st_mode) || S_ISLNK(tempStat.st_mode) || unlink(temp.c_str()) != 0) {
            fclose(input);
            return false;
        }
    }
    const int tempFd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (tempFd < 0) {
        fclose(input);
        return false;
    }
    FILE* output = fdopen(tempFd, "wb");
    if (!output) {
        close(tempFd);
        unlink(temp.c_str());
        fclose(input);
        return false;
    }
    char line[512];
    size_t total = 0;
    bool wroteEnabled = false;
    bool ok = true;
    bool outputHasData = false;
    bool outputEndsWithNewline = true;
    while (fgets(line, sizeof(line), input)) {
        total += strlen(line);
        if (total > kMaxManifestBytes) { ok = false; break; }
        std::string text = trim(line);
        const size_t equal = text.find('=');
        const bool enabledLine = equal != std::string::npos &&
                                 trim(text.substr(0, equal)) == "enabled";
        if (enabledLine) {
            if (!wroteEnabled) {
                if (outputHasData && !outputEndsWithNewline && fputc('\n', output) == EOF) {
                    ok = false;
                    break;
                }
                ok = fprintf(output, "enabled=%d\n", enabled ? 1 : 0) >= 0;
                wroteEnabled = true;
                outputHasData = true;
                outputEndsWithNewline = true;
            }
        } else {
            const size_t lineLength = strlen(line);
            if (fwrite(line, 1, lineLength, output) != lineLength) {
                ok = false;
            } else if (lineLength > 0) {
                outputHasData = true;
                outputEndsWithNewline = line[lineLength - 1] == '\n';
            }
        }
        if (!ok) break;
    }
    if (ferror(input)) ok = false;
    if (ok && !wroteEnabled) {
        if (outputHasData && !outputEndsWithNewline && fputc('\n', output) == EOF) ok = false;
    }
    if (ok && !wroteEnabled) {
        ok = fprintf(output, "enabled=%d\n", enabled ? 1 : 0) >= 0;
    }
    if (fflush(output) != 0) ok = false;
    if (ok && fsync(fileno(output)) != 0) ok = false;
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (ok && rename(temp.c_str(), manifest.c_str()) == 0) return true;
    unlink(temp.c_str());
    return false;
}

}  // namespace

void setGameRoot(const std::string& gameRoot) {
    {
        std::lock_guard<std::mutex> lock(sModsMutex);
        sModsDirectory = gameRoot.empty() ? std::string() : gameRoot + "/mods";
    }
    refresh();
}

void refresh() {
    const std::string directory = modsDirectory();
    if (directory.empty()) return;
    auto scanned = std::make_shared<std::vector<ModInfo>>();
    if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
        port_log("mods: cannot create %s: %s", directory.c_str(), strerror(errno));
        return;
    }
    if (!isPlainDirectory(directory)) {
        port_log("mods: directory is not a plain directory; refusing to scan");
        return;
    }
    DIR* handle = opendir(directory.c_str());
    if (!handle) {
        port_log("mods: cannot open %s: %s", directory.c_str(), strerror(errno));
        return;
    }
    while (dirent* entry = readdir(handle)) {
        if (scanned->size() >= kMaxMods) break;
        const std::string id = entry->d_name;
        if (!validId(id)) continue;
        const std::string root = directory + "/" + id;
        if (!isPlainDirectory(root)) continue;
        ModInfo info;
        info.id = id;
        if (!parseManifest(root + "/mod.ini", info)) {
            port_log("mods: skipped %s (missing or invalid mod.ini)", id.c_str());
            continue;
        }
        info.replacementCount = 0;
        countFilesRecursive(root + "/files", 0, info.replacementCount);
        if (info.replacementCount == 0) {
            port_log("mods: skipped %s (no regular files under files/)", id.c_str());
            continue;
        }
        scanned->push_back(info);
    }
    closedir(handle);
    std::sort(scanned->begin(), scanned->end(), [](const ModInfo& a, const ModInfo& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.id < b.id;
    });
    {
        std::lock_guard<std::mutex> lock(sModsMutex);
        if (sModsDirectory != directory) return;
        sMods = scanned;
    }
    port_log("mods: scanned %zu valid local packs in %s", scanned->size(), directory.c_str());
}

void setGlobalEnabled(bool enabled) { sGlobalEnabled.store(enabled); }

std::shared_ptr<const std::vector<ModInfo>> list() {
    std::lock_guard<std::mutex> lock(sModsMutex);
    return sMods;
}

std::string modsDirectory() {
    std::lock_guard<std::mutex> lock(sModsMutex);
    return sModsDirectory;
}

bool setEnabled(const std::string& id, bool enabled) {
    if (!validId(id)) return false;
    const std::string directory = modsDirectory();
    const auto current = list();
    if (directory.empty()) return false;
    auto found = std::find_if(current->begin(), current->end(),
                              [&](const ModInfo& mod) { return mod.id == id; });
    if (found == current->end()) return false;
    const std::string modRoot = directory + "/" + id;
    const std::string manifest = modRoot + "/mod.ini";
    if (!isPlainDirectory(modRoot) || !rewriteEnabled(manifest, enabled)) {
        port_log("mods: failed to %s pack %s", enabled ? "enable" : "disable", id.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sModsMutex);
        if (sModsDirectory != directory) return false;
        auto updated = std::make_shared<std::vector<ModInfo>>(*sMods);
        auto update = std::find_if(updated->begin(), updated->end(),
                                   [&](const ModInfo& mod) { return mod.id == id; });
        if (update == updated->end()) return false;
        update->enabled = enabled;
        sMods = updated;
    }
    port_log("mods: pack %s %s; file replacements apply on next game start",
             id.c_str(), enabled ? "enabled" : "disabled");
    return true;
}

std::string findReplacement(const std::string& relativeGamePath) {
    if (!sGlobalEnabled.load() || !validRelativePath(relativeGamePath)) return {};
    const std::string directory = modsDirectory();
    const auto mods = list();
    if (directory.empty()) return {};
    std::string replacement;
    for (const ModInfo& mod : *mods) {
        if (!mod.enabled) continue;
        const std::string filesRoot = directory + "/" + mod.id + "/files";
        if (!pathContainsNoSymlink(filesRoot, relativeGamePath, true)) continue;
        replacement = filesRoot + "/" + relativeGamePath;
    }
    return replacement;
}

}  // namespace port_mods
