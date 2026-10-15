#include "settings.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace tethercam::agent {
namespace {

std::filesystem::path XdgDir(const char* var, const char* fallback) {
    if (const char* v = std::getenv(var); v && *v == '/') return std::filesystem::path(v) / "tethercam";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / fallback / "tethercam";
}

}  // namespace

std::filesystem::path Settings::DefaultPath() { return XdgDir("XDG_CONFIG_HOME", ".config") / "settings"; }

std::filesystem::path StateDir() { return XdgDir("XDG_STATE_HOME", ".local/state"); }

Settings::Settings(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) return;
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.starts_with('#')) continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

std::string Settings::Get(const std::string& key) const {
    std::lock_guard lock(mu_);
    auto it = values_.find(key);
    return it == values_.end() ? std::string{} : it->second;
}

void Settings::Set(const std::string& key, const std::string& value) {
    std::lock_guard lock(mu_);
    // Values are single-line by construction (labels and numbers); drop newlines just in case.
    std::string v = value;
    std::erase_if(v, [](char c) { return c == '\n' || c == '\r'; });
    if (values_[key] == v) return;
    values_[key] = v;
    SaveLocked();
}

void Settings::SaveLocked() const {
    if (file_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    // Write a temp file and rename it so a crash never leaves a half-written file.
    const auto tmp = std::filesystem::path(file_).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        for (const auto& [k, v] : values_) out << k << '=' << v << '\n';
        if (!out) return;
    }
    std::filesystem::rename(tmp, file_, ec);
}

}  // namespace tethercam::agent
