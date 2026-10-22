#pragma once

#include <memory>
#include <string>
#include <vector>

namespace port_mods {

struct ModInfo {
    std::string id;
    std::string name;
    std::string version;
    bool enabled = false;
    int priority = 0;
    int replacementCount = 0;
};

// The game keeps its own converted data tree untouched. Mods are optional
// directory overlays rooted at <game root>/mods/<id>/files/.
void setGameRoot(const std::string& gameRoot);
void refresh();
void setGlobalEnabled(bool enabled);
std::shared_ptr<const std::vector<ModInfo>> list();
std::string modsDirectory();
bool setEnabled(const std::string& id, bool enabled);

// Returns an existing regular-file replacement for a path relative to the
// game's files/ directory, or an empty string if no enabled mod replaces it.
std::string findReplacement(const std::string& relativeGamePath);

}  // namespace port_mods
