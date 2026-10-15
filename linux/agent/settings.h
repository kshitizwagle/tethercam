#pragma once

// User choices that survive reconnects and restarts: key=value lines in
// $XDG_CONFIG_HOME/tethercam/settings (default ~/.config/tethercam/settings).

#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace tethercam::agent {

class Settings {
public:
    // An empty path keeps everything in memory (tests, --no-settings).
    explicit Settings(std::filesystem::path file);
    static std::filesystem::path DefaultPath();

    std::string Get(const std::string& key) const;
    void Set(const std::string& key, const std::string& value);

private:
    void SaveLocked() const;

    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> values_;
};

// $XDG_STATE_HOME/tethercam (default ~/.local/state/tethercam), for logs.
std::filesystem::path StateDir();

}  // namespace tethercam::agent
